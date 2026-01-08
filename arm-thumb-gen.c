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

/* Do not invert parameter evaluation order for ARM AAPCS; arguments are
 * laid out left-to-right in registers/stack and inverting breaks 64-bit
 * stack arguments ordering. */
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
                                                "__thumb__\0"
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

/* Additional scratch register exclusions (e.g. to protect argument registers
 * while materializing an indirect call target). Applied on top of per-call
 * exclude masks. */
static uint32_t scratch_global_exclude = 0;

/* Track registers that were PUSH'ed by get_scratch_reg_with_save() in ORDER.
 * We must POP in reverse order since ARM POP with register lists always pops
 * in register-number order, not stack order.
 * Size 128 since same register can be pushed multiple times for complex ops like
 * function calls with many arguments. */
static int scratch_push_stack[128];
static int scratch_push_count = 0;

int is_valid_opcode(thumb_opcode op);
int ot(thumb_opcode op);
int ot_check(thumb_opcode op);
static void load_to_register(int reg, int reg_from, SValue *src);
int th_has_immediate_value(int r);
int load_word_from_base(int ir, int base, int fc, int sign);
static void tcc_gen_machine_load_from_stack(int reg, int offset);
int th_patch_call(int t, int a);
/* Structure to track scratch register allocation with potential save/restore */
typedef struct ScratchRegAlloc
{
  int reg;       /* The allocated scratch register */
  int saved : 1; /* Whether the register was saved to stack */
} ScratchRegAlloc;
ScratchRegAlloc th_offset_to_reg(int offset, int sign);

/* Get a free scratch register using liveness information.
 * exclude_regs is a bitmap of registers that must not be used.
 * If no free register is found, saves R_IP to stack and returns it.
 * Returns ScratchRegAlloc with the register and whether it was saved.
 */
static ScratchRegAlloc get_scratch_reg_with_save(uint32_t exclude_regs)
{
  ScratchRegAlloc result = {0};
  TCCIRState *ir = tcc_state->ir;

  fprintf(stderr, "[SCRATCH] get_scratch_reg: input_exclude=0x%x global_exclude=0x%x\n", exclude_regs,
          scratch_global_exclude);

  exclude_regs |= scratch_global_exclude;

  if (ir)
  {
    int reg = tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, exclude_regs, ir->leaffunc);
    /* tcc_ls_find_free_scratch_reg() returns PREG_NONE (0xFF) if none.
     * Do not treat that as a valid register (it would encode as PC and fault).
     */
    if (reg != PREG_NONE && reg >= 0 && reg < 16)
    {
      fprintf(stderr, "[SCRATCH] -> returning reg=%d (free) exclude=0x%x\n", reg, exclude_regs);
      result.reg = reg;
      result.saved = 0;
      /* Update global exclude so subsequent calls won't return the same register */
      scratch_global_exclude |= (1u << reg);
      return result;
    }
  }

  /* No free register found - we need to save one to the stack */
  /* Prefer R_IP (R12) as it's the inter-procedure scratch register */
  int reg_to_save = -1;
  if (!(exclude_regs & (1 << R_IP)))
  {
    reg_to_save = R_IP;
  }
  else if (ir && ir->leaffunc && !(exclude_regs & (1 << R_LR)))
  {
    /* R_IP is excluded, try R_LR if we're in a leaf function */
    reg_to_save = R_LR;
  }
  else
  {
    /* Try R0-R3 */
    for (int r = 0; r <= 3; ++r)
    {
      if (!(exclude_regs & (1 << r)))
      {
        reg_to_save = r;
        break;
      }
    }
  }

  if (reg_to_save < 0)
  {
    /* Try any register R4-R11 that's not excluded */
    for (int r = 4; r <= 11; ++r)
    {
      if (!(exclude_regs & (1 << r)))
      {
        reg_to_save = r;
        break;
      }
    }
  }

  if (reg_to_save < 0)
  {
    tcc_error("compiler_error: no register available for scratch (all 16 registers excluded)");
  }

  /* No free register found - save one to the stack */
  fprintf(stderr, "[SCRATCH] WARNING: no free scratch register! Saving r%d to stack\n", reg_to_save);
  ot_check(th_push(1 << reg_to_save));
  result.reg = reg_to_save;
  result.saved = 1;
  /* Track push ORDER - we must POP in reverse order since ARM POP with register
   * lists pops in register-number order, not stack order. */
  if (scratch_push_count < 128)
  {
    scratch_push_stack[scratch_push_count++] = reg_to_save;
  }
  else
  {
    tcc_error("compiler_error: scratch register push stack overflow (>128 pushes without restore)");
  }
  /* Do NOT add to global_exclude! The register is now free to use (value saved on stack).
   * If we need another scratch later, we can push the same register again - each push/pop
   * pair is tracked in scratch_push_stack and will be restored in reverse order. */
  return result;
}

/* Restore a scratch register if it was saved */
static void restore_scratch_reg(ScratchRegAlloc *alloc)
{
  if (alloc->saved)
  {
    ot_check(th_pop(1 << alloc->reg));
    alloc->saved = 0;
    /* Remove from push stack - find and remove this register.
     * NOTE: This assumes callers restore in reverse order of allocation.
     * If not, we may corrupt the stack! For safety, only remove if it's
     * the last pushed register. */
    if (scratch_push_count > 0 && scratch_push_stack[scratch_push_count - 1] == alloc->reg)
    {
      scratch_push_count--;
    }
    else if (scratch_push_count > 0)
    {
      fprintf(stderr,
              "[SCRATCH] WARNING: restore_scratch_reg out of order! "
              "reg=%d but top of stack is %d\n",
              alloc->reg, scratch_push_stack[scratch_push_count - 1]);
      /* Still need to find and remove it to avoid double-pop */
      for (int i = scratch_push_count - 1; i >= 0; i--)
      {
        if (scratch_push_stack[i] == alloc->reg)
        {
          /* Shift remaining entries down */
          for (int j = i; j < scratch_push_count - 1; j++)
            scratch_push_stack[j] = scratch_push_stack[j + 1];
          scratch_push_count--;
          break;
        }
      }
    }
  }
  /* Always release from global exclude */
  scratch_global_exclude &= ~(1u << alloc->reg);
}

/* Restore all scratch registers that were pushed but not explicitly restored.
 * Call this at the end of each IR instruction to clean up after callers that
 * used .reg and discarded the saved flag. POP in reverse order of PUSH! */
static void restore_all_pushed_scratch_regs(void)
{
  /* Pop in reverse order - ARM POP with register lists pops in register-number
   * order, so we must issue individual POPs in reverse push order */
  for (int i = scratch_push_count - 1; i >= 0; i--)
  {
    int reg = scratch_push_stack[i];
    fprintf(stderr, "[SCRATCH] auto-restoring r%d (push order %d)\n", reg, i);
    ot_check(th_pop(1 << reg));
  }
  scratch_push_count = 0;
  /* Also reset global exclude for next IR instruction */
  scratch_global_exclude = 0;
}

ST_FUNC void tcc_machine_acquire_scratch(TCCMachineScratchRegs *scratch, unsigned flags)
{
  if (!scratch)
    return;

  scratch->reg_count = 0;
  scratch->saved_mask = 0;
  scratch->regs[0] = PREG_NONE;
  scratch->regs[1] = PREG_NONE;

  uint32_t exclude_regs = 0;
  const int need_pair = (flags & TCC_MACHINE_SCRATCH_NEEDS_PAIR) != 0;

  ScratchRegAlloc first = get_scratch_reg_with_save(exclude_regs);
  if (first.reg == PREG_NONE)
    tcc_error("compiler_error: unable to allocate scratch register");

  scratch->regs[0] = first.reg;
  scratch->reg_count = 1;
  if (first.saved)
    scratch->saved_mask |= 1u;
  exclude_regs |= (1u << first.reg);
  /* Update global exclude so subsequent scratch allocations don't get same register */
  scratch_global_exclude |= (1u << first.reg);

  if (need_pair)
  {
    ScratchRegAlloc second = get_scratch_reg_with_save(exclude_regs);
    if (second.reg == PREG_NONE)
      tcc_error("compiler_error: unable to allocate scratch register pair");

    scratch->regs[1] = second.reg;
    scratch->reg_count = 2;
    if (second.saved)
      scratch->saved_mask |= 2u;
    /* Update global exclude for pair's second register too */
    scratch_global_exclude |= (1u << second.reg);
  }
}

ST_FUNC void tcc_machine_release_scratch(const TCCMachineScratchRegs *scratch)
{
  if (!scratch)
    return;

  /* Clear global exclude bits for released registers so they can be reused */
  for (int i = 0; i < scratch->reg_count; ++i)
  {
    int reg = scratch->regs[i];
    if (reg != PREG_NONE && reg >= 0 && reg < 16)
      scratch_global_exclude &= ~(1u << reg);
  }

  for (int i = scratch->reg_count - 1; i >= 0; --i)
  {
    if (!(scratch->saved_mask & (1u << i)))
      continue;

    int reg = scratch->regs[i];
    if (reg == PREG_NONE)
      continue;

    ot_check(th_pop(1 << reg));
  }
}

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

/* Forward declaration from tccir.c */
int tcc_ir_is_spilled(SValue *sv);
int tcc_ir_is_64bit(int t);

/* Forward declarations for helpers used by spill preloading. */
void load_vt_local(int r, SValue *sv, int base);
int load_short_from_base(int ir, int base, int fc, int sign);
int load_ushort_from_base(int ir, int base, int fc, int sign);
int load_byte_from_base(int ir, int base, int fc, int sign);
int load_ubyte_from_base(int ir, int base, int fc, int sign);

/* Preload spilled operands into scratch registers before an operation.
 * Returns SpillContext with information for store-back.
 * Parameters:
 *   q: The IR quad instruction
 *   preload_src1: Whether to preload src1 if spilled
 *   preload_src2: Whether to preload src2 if spilled
 *   setup_dest: Whether to set up dest register if spilled
 */
SpillContext tcc_ir_preload_spills(TACQuadruple *q, int preload_src1, int preload_src2, int setup_dest)
{
  SpillContext ctx = {0};
  ctx.is_64bit = tcc_ir_is_64bit(q->dest.type.t);
  ctx.dest_scratch_reg = PREG_NONE;
  ctx.dest_scratch_reg1 = PREG_NONE;
  ctx.src1_scratch_reg = PREG_NONE;
  ctx.src1_scratch_reg1 = PREG_NONE;
  ctx.src2_scratch_reg = PREG_NONE;
  ctx.src2_scratch_reg1 = PREG_NONE;
  uint32_t exclude_regs = 0;

  /* Save original register allocations */
  ctx.orig_src1_pr0 = q->src1.pr0;
  ctx.orig_src1_pr1 = q->src1.pr1;
  ctx.orig_src2_pr0 = q->src2.pr0;
  ctx.orig_src2_pr1 = q->src2.pr1;
  ctx.orig_dest_pr0 = q->dest.pr0;
  ctx.orig_dest_pr1 = q->dest.pr1;

  /* Check if src1 is an address-of operation (VT_LOCAL without VT_LVAL).
   * Address-of doesn't need preload - we compute the address directly. */
  int src1_is_address_of = ((q->src1.r & VT_VALMASK) == VT_LOCAL) && !(q->src1.r & VT_LVAL);

  /* Preload src1 if needed */
  if (preload_src1 && tcc_ir_is_spilled(&q->src1) && !th_has_immediate_value(q->src1.r) &&
      !tcc_ir_is_64bit(q->src1.type.t) && !src1_is_address_of)
  {
    ctx.src1_spilled = 1;
    ctx.src1_offset = q->src1.c.i;

    /* DISABLED: Spill cache causes issues with array accesses and recursive functions.
     * The cache doesn't track when memory is modified through pointers. */
    TCCIRState *ir = tcc_state->ir;
    int cached_reg = -1; /* Disabled: always load from stack */

    if (cached_reg >= 0 && !(exclude_regs & (1 << cached_reg)))
    {
      /* Value already in register - no need to load! */
      q->src1.pr0 = cached_reg;
      ctx.src1_scratch_reg = cached_reg;
      exclude_regs |= (1 << cached_reg);
    }
    else
    {
      /* Need to load from stack */
      int scratch = (ir)
                        ? tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, exclude_regs, ir->leaffunc)
                        : PREG_NONE;
      if (scratch == PREG_NONE)
      {
        /* No free register - save R_IP to stack and use it */
        scratch = R_IP;
        if (exclude_regs & (1 << R_IP))
        {
          /* R_IP excluded, try to find another register */
          for (int r = 0; r <= 3; ++r)
          {
            if (!(exclude_regs & (1 << r)))
            {
              scratch = r;
              break;
            }
          }
        }
        ot_check(th_push(1 << scratch));
        ctx.src1_reg_saved = 1;
      }
      ctx.src1_scratch_reg = scratch;

      /* Call load BEFORE modifying pr0 - load() uses pr0 to detect spilled values.
       * The first argument to load() specifies the destination register.
       *
       * IMPORTANT: For spilled VT_LOCAL values, we need to load the VALUE from the
       * spill slot, not compute the address. The `load()` function with VT_LOCAL
       * without VT_LVAL computes address-of, which is wrong for spilled temporaries.
       *
       * For LOAD operations or ASSIGN from spilled TMPs, we directly generate the
       * load instruction instead of going through load() to avoid confusion. */
      int saved_r = q->src1.r;
      int v = q->src1.r & VT_VALMASK;
      int src1_is_temp = TCCIR_DECODE_VREG_TYPE(q->src1.vr) == TCCIR_VREG_TYPE_TEMP;

      /* For spilled VT_LOCAL values (both with and without VT_LVAL), directly load
       * the value from stack instead of using load() which has confusing semantics. */
      if (v == VT_LOCAL || v == VT_LLOCAL)
      {
        /* For LOAD operations, src1 is an address (lvalue).
         * Preloading must materialize an address into a register.
         *
         * There are two distinct cases here:
         * 1) Real locals/arrays: the spill slot is the object storage; we must compute
         *    the address of that storage (otherwise we treat the first bytes as a pointer
         *    and HardFault, e.g. "nonono" -> 0x6f6e6f6e).
         * 2) Spilled temporaries that hold a pointer: the spill slot contains the pointer
         *    value; we must load that pointer value and then dereference it.
         */
        if (q->op == TCCIR_OP_LOAD)
        {
          if (src1_is_temp)
          {
            /* Spill slot holds the pointer value; load it first, then mark as lvalue. */
            int src_offset = q->src1.c.i;
            int orig_offset = src_offset;
            /* For parameters, adjust offset to account for pushed registers */
            if (saved_r & VT_PARAM)
            {
              src_offset += offset_to_args;
            }
            int src_sign = (src_offset < 0);
            int src_abs = src_sign ? -src_offset : src_offset;
            if (!load_word_from_base(scratch, R_FP, src_abs, src_sign))
            {
              ScratchRegAlloc rr_alloc = th_offset_to_reg(src_abs, src_sign);
              int rr = rr_alloc.reg;
              ot_check(th_ldr_reg(scratch, R_FP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
              restore_scratch_reg(&rr_alloc);
            }
            q->src1.r = scratch | VT_LVAL;
            q->src1.c.i = 0;
          }
          else
          {
            /* Non-temporary VT_LOCAL/VT_LLOCAL stack slots represent concrete storage.
             * For a LOAD op, src1 denotes the memory location we want to read.
             * Always materialize the ADDRESS of the stack slot (FP/SP + offset), then
             * let the LOAD dereference it once.
             *
             * Treating pointer-typed slots as "pointer values that must be dereferenced"
             * would introduce an extra indirection (double-deref), which breaks VLA base
             * pointers (e.g. indexing via a VLA base stored in a local slot).
             */
            int base = R_FP;
            if (tcc_state->need_frame_pointer == 0)
              base = R_SP;

            SValue addr = q->src1;
            addr.r &= ~VT_LVAL; /* VT_LOCAL without VT_LVAL means address-of */
            if (saved_r & VT_PARAM)
              addr.c.i += offset_to_args;
            load_vt_local(scratch, &addr, base);
            q->src1.r = scratch | VT_LVAL; /* address in register, needs dereference */
            q->src1.c.i = 0;               /* base already points to exact address */
          }
        }
        else
        {
          /* Load value from stack with the correct width/sign based on type. */
          int src_offset = q->src1.c.i;
          int orig_offset = src_offset;
          /* For parameters, adjust offset to account for pushed registers */
          if (saved_r & VT_PARAM)
          {
            src_offset += offset_to_args;
          }
          int src_sign = (src_offset < 0);
          int src_abs = src_sign ? -src_offset : src_offset;
          int ft = q->src1.type.t;
          int btype = ft & VT_BTYPE;
          int ok = 0;

          if (btype == VT_SHORT)
          {
            if (ft & VT_UNSIGNED)
              ok = load_ushort_from_base(scratch, R_FP, src_abs, src_sign);
            else
              ok = load_short_from_base(scratch, R_FP, src_abs, src_sign);
          }
          else if (btype == VT_BYTE || btype == VT_BOOL)
          {
            if (ft & VT_UNSIGNED)
              ok = load_ubyte_from_base(scratch, R_FP, src_abs, src_sign);
            else
              ok = load_byte_from_base(scratch, R_FP, src_abs, src_sign);
          }
          else
          {
            ok = load_word_from_base(scratch, R_FP, src_abs, src_sign);
          }

          if (!ok)
          {
            ScratchRegAlloc rr_alloc = th_offset_to_reg(src_abs, src_sign);
            int rr = rr_alloc.reg;
            ot_check(th_ldr_reg(scratch, R_FP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
            restore_scratch_reg(&rr_alloc);
          }

          q->src1.r = scratch; /* value in register */
          q->src1.c.i = 0;
        }
      }
      else
      {
        /* For non-LOCAL cases, use normal load.
         * For global symbols (VT_CONST | VT_SYM) with VT_LVAL, we need to:
         * 1. Load the address into a register (stripping VT_LVAL for the load call)
         * 2. For LOAD operations, preserve VT_LVAL so we dereference later
         * 3. For ASSIGN operations, dereference immediately */
        int had_lval = saved_r & VT_LVAL;
        q->src1.r &= ~VT_LVAL; /* Strip VT_LVAL for the load call */
        load(scratch, &q->src1);
        /* For ASSIGN ops with VT_LVAL: dereference now by loading from the address */
        if (q->op == TCCIR_OP_ASSIGN && had_lval)
        {
          /* scratch now contains the address, dereference it */
          ot_check(th_ldr_imm(scratch, scratch, 0, 6, ENFORCE_ENCODING_NONE));
          q->src1.r = scratch; /* Value loaded, no more VT_LVAL */
        }
        /* For LOAD ops on global symbols: address is now in register, needs dereference */
        else if (q->op == TCCIR_OP_LOAD && had_lval)
        {
          q->src1.r = scratch | VT_LVAL; /* Address in register, needs dereference */
        }
        else
        {
          q->src1.r = scratch; /* Value is in this register now */
        }
      }
      q->src1.pr0 = scratch;
      exclude_regs |= (1 << scratch);

      /* DISABLED: Don't record in spill cache - causes issues */
#if 0
      /* Record in cache that this register now holds this stack slot
       * BUT only for true spills, not local variables */
      if (ir && (ctx.orig_src1_pr0 & PREG_SPILLED))
      {
        tcc_ir_spill_cache_record(&ir->spill_cache, scratch, ctx.src1_offset);
      }
#endif
    }
  }

  /* Preload 64-bit src1 if needed */
  if (preload_src1 && tcc_ir_is_spilled(&q->src1) && !th_has_immediate_value(q->src1.r) &&
      tcc_ir_is_64bit(q->src1.type.t) && !src1_is_address_of)
  {
    ctx.src1_spilled = 1;
    ctx.src1_offset = q->src1.c.i;

    TCCIRState *ir = tcc_state->ir;

    int scratch_lo =
        (ir) ? tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, exclude_regs, ir->leaffunc)
             : PREG_NONE;
    if (scratch_lo == PREG_NONE)
    {
      scratch_lo = R_IP;
      if (exclude_regs & (1u << R_IP))
      {
        for (int r = 0; r <= 3; ++r)
        {
          if (!(exclude_regs & (1u << r)))
          {
            scratch_lo = r;
            break;
          }
        }
      }
      ot_check(th_push(1 << scratch_lo));
      ctx.src1_reg_saved = 1;
    }
    ctx.src1_scratch_reg = scratch_lo;
    exclude_regs |= (1u << scratch_lo);

    int scratch_hi =
        (ir) ? tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, exclude_regs, ir->leaffunc)
             : PREG_NONE;
    if (scratch_hi == PREG_NONE)
    {
      scratch_hi = R_IP;
      if (exclude_regs & (1u << R_IP))
      {
        for (int r = 0; r <= 3; ++r)
        {
          if (!(exclude_regs & (1u << r)))
          {
            scratch_hi = r;
            break;
          }
        }
      }
      ot_check(th_push(1 << scratch_hi));
      ctx.src1_reg_saved1 = 1;
    }
    ctx.src1_scratch_reg1 = scratch_hi;
    exclude_regs |= (1u << scratch_hi);

    int off_lo = q->src1.c.i;
    int sign_lo = (off_lo < 0);
    int abs_lo = sign_lo ? -off_lo : off_lo;
    if (!load_word_from_base(scratch_lo, R_FP, abs_lo, sign_lo))
    {
      ScratchRegAlloc rr_alloc_lo = th_offset_to_reg(abs_lo, sign_lo);
      int rr = rr_alloc_lo.reg;
      ot_check(th_ldr_reg(scratch_lo, R_FP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      restore_scratch_reg(&rr_alloc_lo);
    }

    int off_hi = off_lo + 4;
    int sign_hi = (off_hi < 0);
    int abs_hi = sign_hi ? -off_hi : off_hi;
    if (!load_word_from_base(scratch_hi, R_FP, abs_hi, sign_hi))
    {
      ScratchRegAlloc rr_alloc_hi = th_offset_to_reg(abs_hi, sign_hi);
      int rr = rr_alloc_hi.reg;
      ot_check(th_ldr_reg(scratch_hi, R_FP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      restore_scratch_reg(&rr_alloc_hi);
    }

    q->src1.r = scratch_lo;
    q->src1.pr0 = scratch_lo;
    q->src1.pr1 = scratch_hi;
    q->src1.c.i = 0;
  }

  /* Preload src2 if needed */
  if (preload_src2 && tcc_ir_is_spilled(&q->src2) && !th_has_immediate_value(q->src2.r) &&
      !tcc_ir_is_64bit(q->src2.type.t))
  {
    ctx.src2_spilled = 1;
    ctx.src2_offset = q->src2.c.i;

    /* DISABLED: Spill cache causes issues - always load from stack */
    TCCIRState *ir = tcc_state->ir;
    int cached_reg = -1; /* Disabled */

    if (cached_reg >= 0 && !(exclude_regs & (1 << cached_reg)))
    {
      /* Value already in register - no need to load! */
      q->src2.pr0 = cached_reg;
      ctx.src2_scratch_reg = cached_reg;
      exclude_regs |= (1 << cached_reg);
    }
    else
    {
      /* Need to load from stack */
      int scratch = (ir)
                        ? tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, exclude_regs, ir->leaffunc)
                        : PREG_NONE;
      if (scratch == PREG_NONE)
      {
        /* No free register - save one to stack and use it */
        scratch = R_IP;
        if (exclude_regs & (1 << R_IP))
        {
          for (int r = 0; r <= 3; ++r)
          {
            if (!(exclude_regs & (1 << r)))
            {
              scratch = r;
              break;
            }
          }
        }
        ot_check(th_push(1 << scratch));
        ctx.src2_reg_saved = 1;
      }
      ctx.src2_scratch_reg = scratch;

      /* Call load BEFORE modifying pr0 - load() uses pr0 to detect spilled values.
       * Same VT_LVAL handling as src1. */
      int saved_r = q->src2.r;
      int v = q->src2.r & VT_VALMASK;
      if (v != VT_LOCAL && v != VT_LLOCAL)
      {
        q->src2.r &= ~VT_LVAL;
      }
      load(scratch, &q->src2);
      q->src2.r = saved_r;
      q->src2.pr0 = scratch;
      exclude_regs |= (1 << scratch);

      /* DISABLED: Don't record in spill cache */
#if 0
      /* Record in cache but only for true spills */
      if (ir && (ctx.orig_src2_pr0 & PREG_SPILLED))
      {
        tcc_ir_spill_cache_record(&ir->spill_cache, scratch, ctx.src2_offset);
      }
#endif
    }
  }

  /* Preload 64-bit src2 if needed */
  if (preload_src2 && tcc_ir_is_spilled(&q->src2) && !th_has_immediate_value(q->src2.r) &&
      tcc_ir_is_64bit(q->src2.type.t))
  {
    ctx.src2_spilled = 1;
    ctx.src2_offset = q->src2.c.i;

    TCCIRState *ir = tcc_state->ir;

    int scratch_lo =
        (ir) ? tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, exclude_regs, ir->leaffunc)
             : PREG_NONE;
    if (scratch_lo == PREG_NONE)
    {
      scratch_lo = R_IP;
      if (exclude_regs & (1u << R_IP))
      {
        for (int r = 0; r <= 3; ++r)
        {
          if (!(exclude_regs & (1u << r)))
          {
            scratch_lo = r;
            break;
          }
        }
      }
      ot_check(th_push(1 << scratch_lo));
      ctx.src2_reg_saved = 1;
    }
    ctx.src2_scratch_reg = scratch_lo;
    exclude_regs |= (1u << scratch_lo);

    int scratch_hi =
        (ir) ? tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, exclude_regs, ir->leaffunc)
             : PREG_NONE;
    if (scratch_hi == PREG_NONE)
    {
      scratch_hi = R_IP;
      if (exclude_regs & (1u << R_IP))
      {
        for (int r = 0; r <= 3; ++r)
        {
          if (!(exclude_regs & (1u << r)))
          {
            scratch_hi = r;
            break;
          }
        }
      }
      ot_check(th_push(1 << scratch_hi));
      ctx.src2_reg_saved1 = 1;
    }
    ctx.src2_scratch_reg1 = scratch_hi;
    exclude_regs |= (1u << scratch_hi);

    int off_lo = q->src2.c.i;
    int sign_lo = (off_lo < 0);
    int abs_lo = sign_lo ? -off_lo : off_lo;
    if (!load_word_from_base(scratch_lo, R_FP, abs_lo, sign_lo))
    {
      ScratchRegAlloc rr_alloc_lo = th_offset_to_reg(abs_lo, sign_lo);
      int rr = rr_alloc_lo.reg;
      ot_check(th_ldr_reg(scratch_lo, R_FP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      restore_scratch_reg(&rr_alloc_lo);
    }

    int off_hi = off_lo + 4;
    int sign_hi = (off_hi < 0);
    int abs_hi = sign_hi ? -off_hi : off_hi;
    if (!load_word_from_base(scratch_hi, R_FP, abs_hi, sign_hi))
    {
      ScratchRegAlloc rr_alloc_hi = th_offset_to_reg(abs_hi, sign_hi);
      int rr = rr_alloc_hi.reg;
      ot_check(th_ldr_reg(scratch_hi, R_FP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      restore_scratch_reg(&rr_alloc_hi);
    }

    q->src2.r = scratch_lo;
    q->src2.pr0 = scratch_lo;
    q->src2.pr1 = scratch_hi;
    q->src2.c.i = 0;
  }

  /* Setup dest if needed.
   * IMPORTANT: Only do this for truly spilled vregs, NOT for memory destinations.
   * A memory destination (VT_LOCAL | VT_LVAL or VT_CONST | VT_SYM | VT_LVAL) doesn't
   * need scratch registers for the destination - we write directly to memory. */
  int dest_is_memory_location_32 = (q->dest.r & VT_LVAL) != 0;
  if (setup_dest && tcc_ir_is_spilled(&q->dest) && !tcc_ir_is_64bit(q->dest.type.t) && !dest_is_memory_location_32)
  {
    ctx.dest_spilled = 1;
    ctx.dest_offset = q->dest.c.i;

    /* Find a free scratch register for dest */
    TCCIRState *ir = tcc_state->ir;
    int scratch = (ir) ? tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, exclude_regs, ir->leaffunc)
                       : PREG_NONE;
    if (scratch == PREG_NONE)
    {
      /* No free register - save one to stack and use it */
      scratch = R_IP;
      if (exclude_regs & (1 << R_IP))
      {
        for (int r = 0; r <= 3; ++r)
        {
          if (!(exclude_regs & (1 << r)))
          {
            scratch = r;
            break;
          }
        }
      }
      ot_check(th_push(1 << scratch));
      ctx.dest_reg_saved = 1;
    }

    q->dest.pr0 = scratch;
    ctx.dest_scratch_reg = scratch; /* Save the scratch register used for storing back */

    /* Invalidate cache entry for this register - it will be overwritten */
    if (ir)
    {
      tcc_ir_spill_cache_invalidate_reg(&ir->spill_cache, scratch);
    }
  }

  /* Setup 64-bit dest if needed.
   * IMPORTANT: Only do this for truly spilled vregs, NOT for memory destinations.
   * A memory destination (VT_LOCAL | VT_LVAL or VT_CONST | VT_SYM | VT_LVAL) doesn't
   * need scratch registers for the destination - we write directly to memory. */
  int dest_is_memory_location = (q->dest.r & VT_LVAL) != 0;
  if (setup_dest && tcc_ir_is_spilled(&q->dest) && tcc_ir_is_64bit(q->dest.type.t) && !dest_is_memory_location)
  {
    ctx.dest_spilled = 1;
    ctx.dest_offset = q->dest.c.i;

    TCCIRState *ir = tcc_state->ir;
    int scratch_lo =
        (ir) ? tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, exclude_regs, ir->leaffunc)
             : PREG_NONE;
    if (scratch_lo == PREG_NONE)
    {
      scratch_lo = R_IP;
      if (exclude_regs & (1u << R_IP))
      {
        for (int r = 0; r <= 3; ++r)
        {
          if (!(exclude_regs & (1u << r)))
          {
            scratch_lo = r;
            break;
          }
        }
      }
      ot_check(th_push(1 << scratch_lo));
      ctx.dest_reg_saved = 1;
    }
    ctx.dest_scratch_reg = scratch_lo;
    exclude_regs |= (1u << scratch_lo);

    int scratch_hi =
        (ir) ? tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, exclude_regs, ir->leaffunc)
             : PREG_NONE;
    if (scratch_hi == PREG_NONE)
    {
      scratch_hi = R_IP;
      if (exclude_regs & (1u << R_IP))
      {
        for (int r = 0; r <= 3; ++r)
        {
          if (!(exclude_regs & (1u << r)))
          {
            scratch_hi = r;
            break;
          }
        }
      }
      ot_check(th_push(1 << scratch_hi));
      ctx.dest_reg_saved1 = 1;
    }
    ctx.dest_scratch_reg1 = scratch_hi;
    exclude_regs |= (1u << scratch_hi);

    q->dest.pr0 = scratch_lo;
    q->dest.pr1 = scratch_hi;

    if (ir)
    {
      tcc_ir_spill_cache_invalidate_reg(&ir->spill_cache, scratch_lo);
      tcc_ir_spill_cache_invalidate_reg(&ir->spill_cache, scratch_hi);
    }
  }

  return ctx;
}

/* Restore any scratch registers that were saved during preload */
void tcc_ir_restore_saved_scratch_regs(SpillContext *ctx)
{
  /* Restore in reverse order of saving (LIFO) */
  if (ctx->dest_reg_saved1 && ctx->dest_scratch_reg1 >= 0)
  {
    ot_check(th_pop(1 << ctx->dest_scratch_reg1));
    ctx->dest_reg_saved1 = 0;
  }
  if (ctx->dest_reg_saved && ctx->dest_scratch_reg >= 0)
  {
    ot_check(th_pop(1 << ctx->dest_scratch_reg));
    ctx->dest_reg_saved = 0;
  }
  if (ctx->src2_reg_saved1 && ctx->src2_scratch_reg1 >= 0)
  {
    ot_check(th_pop(1 << ctx->src2_scratch_reg1));
    ctx->src2_reg_saved1 = 0;
  }
  if (ctx->src2_reg_saved && ctx->src2_scratch_reg >= 0)
  {
    ot_check(th_pop(1 << ctx->src2_scratch_reg));
    ctx->src2_reg_saved = 0;
  }
  if (ctx->src1_reg_saved1 && ctx->src1_scratch_reg1 >= 0)
  {
    ot_check(th_pop(1 << ctx->src1_scratch_reg1));
    ctx->src1_reg_saved1 = 0;
  }
  if (ctx->src1_reg_saved && ctx->src1_scratch_reg >= 0)
  {
    ot_check(th_pop(1 << ctx->src1_scratch_reg));
    ctx->src1_reg_saved = 0;
  }
}

/* Spill cache management functions for avoiding redundant loads */

void tcc_ir_spill_cache_clear(SpillCache *cache)
{
  for (int i = 0; i < SPILL_CACHE_SIZE; i++)
  {
    cache->entries[i].valid = 0;
  }
}

void tcc_ir_spill_cache_record(SpillCache *cache, int reg, int offset)
{
  /* First invalidate any existing entry for this register or offset */
  tcc_ir_spill_cache_invalidate_reg(cache, reg);
  tcc_ir_spill_cache_invalidate_offset(cache, offset);

  /* Find empty slot or oldest entry to replace */
  for (int i = 0; i < SPILL_CACHE_SIZE; i++)
  {
    if (!cache->entries[i].valid)
    {
      cache->entries[i].valid = 1;
      cache->entries[i].reg = reg;
      cache->entries[i].offset = offset;
      return;
    }
  }
  /* Cache full - replace first entry (simple eviction) */
  cache->entries[0].valid = 1;
  cache->entries[0].reg = reg;
  cache->entries[0].offset = offset;
}

int tcc_ir_spill_cache_lookup(SpillCache *cache, int offset)
{
  for (int i = 0; i < SPILL_CACHE_SIZE; i++)
  {
    if (cache->entries[i].valid && cache->entries[i].offset == offset)
    {
      return cache->entries[i].reg;
    }
  }
  return -1; /* Not found */
}

void tcc_ir_spill_cache_invalidate_reg(SpillCache *cache, int reg)
{
  for (int i = 0; i < SPILL_CACHE_SIZE; i++)
  {
    if (cache->entries[i].valid && cache->entries[i].reg == reg)
    {
      cache->entries[i].valid = 0;
    }
  }
}

void tcc_ir_spill_cache_invalidate_offset(SpillCache *cache, int offset)
{
  for (int i = 0; i < SPILL_CACHE_SIZE; i++)
  {
    if (cache->entries[i].valid && cache->entries[i].offset == offset)
    {
      cache->entries[i].valid = 0;
    }
  }
}

/* Store back a spilled destination after operation completes */
void tcc_ir_storeback_spill(TACQuadruple *q, SpillContext *ctx)
{
  if (ctx->dest_spilled && !tcc_ir_is_64bit(q->dest.type.t))
  {
    q->dest.pr0 = ctx->orig_dest_pr0;
    q->dest.r = VT_LOCAL;
    q->dest.c.i = ctx->dest_offset;

    /* Use the scratch register that was assigned during preload and contains the result */
    int scratch = ctx->dest_scratch_reg;
    if (scratch == PREG_NONE)
    {
      /* Fallback: should not happen if preload was called correctly */
      TCCIRState *ir = tcc_state->ir;
      uint32_t exclude_regs = 0;
      scratch = (ir) ? tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, exclude_regs, ir->leaffunc)
                     : PREG_NONE;
      if (scratch == PREG_NONE)
      {
        /* Emergency fallback - save R_IP, use it, restore it */
        scratch = R_IP;
        ot_check(th_push(1 << scratch));
        store(scratch, &q->dest);
        ot_check(th_pop(1 << scratch));
        return;
      }
    }

    store(scratch, &q->dest);

    /* DISABLED: Don't record in spill cache - causes issues with arrays/pointers */
#if 0
    /* Record in spill cache that this register now holds this stack slot value
     * BUT only for true spills (PREG_SPILLED), not for local variables */
    TCCIRState *ir = tcc_state->ir;
    if (ir && (ctx->orig_dest_pr0 & PREG_SPILLED))
    {
      tcc_ir_spill_cache_record(&ir->spill_cache, scratch, ctx->dest_offset);
    }
#endif
  }

  if (ctx->dest_spilled && tcc_ir_is_64bit(q->dest.type.t))
  {
    q->dest.pr0 = ctx->orig_dest_pr0;
    q->dest.pr1 = ctx->orig_dest_pr1;
    q->dest.r = VT_LOCAL;
    q->dest.c.i = ctx->dest_offset;

    int scratch_lo = ctx->dest_scratch_reg;
    int scratch_hi = ctx->dest_scratch_reg1;
    if (scratch_lo == PREG_NONE || scratch_hi == PREG_NONE)
    {
      /* Fallback: should not happen if preload was called correctly */
      tcc_error("compiler_error: missing scratch regs for 64-bit storeback");
    }
    else
    {
      SValue dest_low = q->dest;
      SValue dest_high = q->dest;
      dest_low.type.t = (dest_low.type.t & ~VT_BTYPE) | (VT_INT | (dest_low.type.t & VT_UNSIGNED));
      dest_high.type.t = dest_low.type.t;
      dest_high.c.i += 4;
      store(scratch_lo, &dest_low);
      store(scratch_hi, &dest_high);
    }
  }

  /* Restore any saved scratch registers after the store is done */
  tcc_ir_restore_saved_scratch_regs(ctx);
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

static uint32_t mapcc(int cc)
{
  /* In most places we carry high-level TOK_* comparisons (TOK_EQ, TOK_LT, ...).
   * Some IR lowering paths may already store an ARM condition code nibble
   * (0..13) in q->src1.c.i. Accept both forms here.
   */
  if ((unsigned)cc <= 0xD)
    return (uint32_t)cc;

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
  tcc_error("unexpected condition code: %d (0x%x)", cc, cc);
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

static void th_literal_pool_init()
{
  thumb_gen_state.literal_pool_size = 64;
  thumb_gen_state.literal_pool_count = 0;
  if (thumb_gen_state.literal_pool)
  {
    tcc_free(thumb_gen_state.literal_pool);
  }
  thumb_gen_state.literal_pool = tcc_mallocz(sizeof(ThumbLiteralPoolEntry) * thumb_gen_state.literal_pool_size);
  thumb_gen_state.generating_function = 0;
  thumb_gen_state.code_size = 0;
  thumb_gen_state.cached_global_sym = NULL;
  thumb_gen_state.cached_global_reg = PREG_NONE;
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

  /* Always reserve R7 (FP) and never allocate it as a general register.
   * The backend relies on a stable FP for FP-relative stack accesses.
   */

  th_literal_pool_init();
}

ST_FUNC void arm_deinit(struct TCCState *s)
{
  (void)s;
  tcc_free(thumb_gen_state.literal_pool);
  thumb_gen_state.literal_pool = NULL;
  thumb_gen_state.literal_pool_size = 0;
  thumb_gen_state.literal_pool_count = 0;
  thumb_gen_state.generating_function = 0;
  thumb_gen_state.code_size = 0;
  thumb_gen_state.cached_global_sym = NULL;
  thumb_gen_state.cached_global_reg = PREG_NONE;
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
  static int pool_seq = 0;

  if (generating_pool)
    return;

  if (thumb_gen_state.literal_pool_count == 0)
  {
    thumb_gen_state.code_size = 0;
    return;
  }

  generating_pool = 1;
  const int this_pool = ++pool_seq;

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

  uint16_t branch_hw0_before = 0, branch_hw1_before = 0;
  uint16_t branch_hw0_after = 0, branch_hw1_after = 0;

  if (thumb_gen_state.generating_function)
  {
    /* Emit placeholder branch (will be patched later) - use 32-bit B.W */
    o(0xf000); /* first halfword of B.W */
    o(0x9000); /* second halfword placeholder */

    /* Snapshot placeholder encoding so we can detect later clobbers */
    branch_hw0_before = *(uint16_t *)(cur_text_section->data + branch_pos);
    branch_hw1_before = *(uint16_t *)(cur_text_section->data + branch_pos + 2);
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

  /* Patch the branch instruction to jump to after the pool.
   * Use the computed pool size rather than (ind - branch_pos), because `ind`
   * can be perturbed by other codepaths and must not affect the local skip.
   * Offset is relative to PC (branch_pos + 4).
   */
  if (thumb_gen_state.generating_function)
  {
    const int branch_after_pool =
        pool_size + need_align; // ind - branch_pos - 4; // branch_pos + 4 + need_align + pool_size;
    // th_patch_call(branch_pos, branch_after_pool);
    thumb_opcode branch = th_b_t4(branch_after_pool);
    uint16_t *branch_patch = (uint16_t *)(cur_text_section->data + branch_pos);
    branch_patch[0] = (branch.opcode >> 16) & 0xffff;
    branch_patch[1] = branch.opcode & 0xffff;

    branch_hw0_after = *(uint16_t *)(cur_text_section->data + branch_pos);
    branch_hw1_after = *(uint16_t *)(cur_text_section->data + branch_pos + 2);

    printf("literal_pool[%d]: branch_pos=0x%x need_align=%d pool_size=%d count=%d branch=%04x %04x, jump: 0x%x\n",
           this_pool, branch_pos, need_align, pool_size, thumb_gen_state.literal_pool_count, branch_hw0_after,
           branch_hw1_after, branch_after_pool);
  }
  th_sym_t();

  /* Second pass: patch all instructions to point to correct literal position */
  for (int i = 0; i < thumb_gen_state.literal_pool_count; i++)
  {
    ThumbLiteralPoolEntry *entry = &thumb_gen_state.literal_pool[i];
    int literal_pos = literal_positions[i];
    int aligned_position = ((literal_pos - entry->patch_position) + 3) & ~3;

    uint16_t b0_prev = 0, b1_prev = 0;
    if (thumb_gen_state.generating_function)
    {
      b0_prev = *(uint16_t *)(cur_text_section->data + branch_pos);
      b1_prev = *(uint16_t *)(cur_text_section->data + branch_pos + 2);
    }

    /* Debug: detect if this patch write overlaps the pool skip-branch */
    if (thumb_gen_state.generating_function && tcc_state && tcc_state->verbose)
    {
      const int branch_start = branch_pos;
      const int branch_end = branch_pos + 4;
      const int p0 = entry->patch_position;
      const int p1 = entry->patch_position + 2;
      int overlaps = 0;
      if (entry->short_instruction)
      {
        overlaps |= (p0 >= branch_start && p0 < branch_end);
      }
      else if (entry->data_size == 8)
      {
        overlaps |= (p0 >= branch_start && p0 < branch_end);
        overlaps |= (p1 >= branch_start && p1 < branch_end);
      }
      else
      {
        overlaps |= (p1 >= branch_start && p1 < branch_end);
      }

      if (overlaps)
      {
        printf("literal_pool[%d]: entry %d patch overlaps branch: patch_pos=0x%x short=%d data_size=%d branch_pos=0x%x",
               this_pool, i, entry->patch_position, entry->short_instruction, entry->data_size, branch_pos);
      }
    }

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

    if (thumb_gen_state.generating_function && tcc_state && tcc_state->verbose)
    {
      uint16_t b0_now = *(uint16_t *)(cur_text_section->data + branch_pos);
      uint16_t b1_now = *(uint16_t *)(cur_text_section->data + branch_pos + 2);
      if (b0_now != b0_prev || b1_now != b1_prev)
      {
        tcc_warning("literal_pool[%d]: branch modified during 2nd pass by entry %d (patch_pos=0x%x short=%d "
                    "data_size=%d): %04x %04x -> %04x %04x\n",
                    this_pool, i, entry->patch_position, entry->short_instruction, entry->data_size, b0_prev, b1_prev,
                    b0_now, b1_now);
      }
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
    return th_mvn_imm(r, 0, -imm - 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  }
  return th_mov_imm(r, imm, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
}
static ScratchRegAlloc th_offset_to_reg_ex(int off, int sign, uint32_t exclude_regs)
{
  /* Find a free scratch register (must not clobber excluded regs).
   * Returns ScratchRegAlloc struct so caller can manage cleanup.
   * Caller MUST call restore_scratch_reg() when done with the register. */
  ScratchRegAlloc alloc = get_scratch_reg_with_save(exclude_regs);
  int rr = alloc.reg;

  /* If mov is not possible then load from data */
  if (!ot(th_generic_mov_imm(rr, off)))
  {
    load_full_const(rr, PREG_NONE, sign ? -off : off, NULL);
    return alloc;
  }

  if (sign)
    ot_check(th_rsb_imm(rr, rr, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  return alloc;
}

ScratchRegAlloc th_offset_to_reg(int off, int sign)
{
  return th_offset_to_reg_ex(off, sign, 0);
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
  /* Computed goto: vtop contains the target address (a pointer value).
   * In IR mode, this must be an *indirect* jump (BX reg), not a direct
   * IR jump-to-instruction-index. */
  tcc_ir_load_if_lvalue(tcc_state->ir, vtop);
  {
    SValue target = *vtop;
    tcc_ir_put(tcc_state->ir, TCCIR_OP_IJUMP, &target, NULL, NULL);
  }
  vtop--;
  print_vstack("ggoto");
}

ST_FUNC void tcc_gen_machine_indirect_jump_op(TACQuadruple *q)
{
  /* Indirect jump: target address in src1 register */
  if (q->src1.pr0 == PREG_NONE || (q->src1.pr0 & PREG_SPILLED))
  {
    tcc_error("internal error: IJUMP target not in a register");
  }
  ot_check(th_bx_reg((uint16_t)q->src1.pr0));
}

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
  /* vtop holds the allocation size in bytes. Adjust SP down by that runtime
   * size and align it to at least 8 bytes.
   *
   * This follows the classic TCC scheme:
   *   r = sp - size
   *   r = r & ~(align-1)
   *   sp = r
   *
   * The size expression is consumed from the value stack.
   */
  (void)type;

  int r = gv(RC_INT);

  /* r = SP - r */
  ot_check(th_sub_reg(r, R_SP, r, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));

  if (align < 8)
    align = 8;
  if (align & (align - 1))
    tcc_error("alignment is not a power of 2: %i", align);

  if (align > 1)
  {
    /* Try immediate BIC first; if it doesn't encode, fall back to register mask. */
    if (!ot(th_bic_imm(r, r, (uint32_t)(align - 1), FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)))
    {
      ScratchRegAlloc mask_alloc = get_scratch_reg_with_save(1u << r);
      int mask_reg = mask_alloc.reg;
      if (!ot(th_generic_mov_imm(mask_reg, align - 1)))
      {
        load_full_const(mask_reg, PREG_NONE, align - 1, NULL);
      }
      ot_check(th_bic_reg(r, r, mask_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      if (mask_alloc.saved)
      {
        ot_check(th_pop(1u << mask_reg));
      }
    }
  }

  /* SP = r */
  ot_check(th_mov_reg(R_SP, r, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));

  vpop();
}

ST_FUNC void gen_vla_sp_save(int addr)
{
  if (nocode_wanted)
    return;

  SValue slot;
  memset(&slot, 0, sizeof(slot));
  slot.type.t = VT_PTR;
  slot.r = VT_LOCAL | VT_LVAL;
  slot.c.i = addr;
  slot.vr = -1;

  /* Save SP into the requested slot via IP scratch to avoid STR SP quirks. */
  ot_check(th_mov_reg(R_IP, R_SP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
  store(R_IP, &slot);
}

ST_FUNC void gen_vla_sp_restore(int addr)
{
  if (nocode_wanted)
    return;

  SValue slot;
  memset(&slot, 0, sizeof(slot));
  slot.type.t = VT_PTR;
  slot.r = VT_LOCAL | VT_LVAL;
  slot.c.i = addr;
  slot.vr = -1;

  load(R_IP, &slot);
  ot_check(th_mov_reg(R_SP, R_IP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
}

int load_ushort_from_base(int ir, int base, int fc, int sign)
{
  const thumb_opcode ins = th_ldrh_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Load ushort sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc, sign);
  return ot(ins);
}

int load_byte_from_base(int ir, int base, int fc, int sign)
{
  const thumb_opcode ins = th_ldrsb_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Load byte sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc, sign);
  return ot(ins);
}

int load_ubyte_from_base(int ir, int base, int fc, int sign)
{
  const thumb_opcode ins = th_ldrb_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Load ubyte sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc, sign);
  return ot(ins);
}

int load_word_from_base(int ir, int base, int fc, int sign)
{
  const thumb_opcode ins = th_ldr_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Load word sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc, sign);
  return ot(ins);
}

int store_word_to_base(int ir, int base, int fc, int sign)
{
  const thumb_opcode ins = th_str_imm(ir, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  TRACE("Store word sign: %d, r %d, base: %d, fc: %d\n", sign, ir, base, fc, sign);
  return ot(ins);
}

ST_FUNC void tcc_machine_load_spill_slot(int dest_reg, int frame_offset)
{
  if (dest_reg == PREG_NONE)
    tcc_error("compiler_error: load_spill_slot requires a destination register");

  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
  const int sign = (frame_offset < 0);
  const int abs_offset = sign ? -frame_offset : frame_offset;

  if (!load_word_from_base(dest_reg, base_reg, abs_offset, sign))
  {
    ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(abs_offset, sign, (1u << dest_reg) | (1u << base_reg));
    int rr = rr_alloc.reg;
    ot_check(th_ldr_reg(dest_reg, base_reg, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&rr_alloc);
  }
}

ST_FUNC void tcc_machine_store_spill_slot(int src_reg, int frame_offset)
{
  if (src_reg == PREG_NONE)
    tcc_error("compiler_error: store_spill_slot requires a source register");

  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
  const int sign = (frame_offset < 0);
  const int abs_offset = sign ? -frame_offset : frame_offset;

  if (!store_word_to_base(src_reg, base_reg, abs_offset, sign))
  {
    ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(abs_offset, sign, (1u << src_reg) | (1u << base_reg));
    int rr = rr_alloc.reg;
    ot_check(th_str_reg(src_reg, base_reg, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&rr_alloc);
  }
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

// are those offsets to allow TREG_R0 start from other register than r0?
// not sure

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
      /* Check if pr0 is valid (not PREG_NONE and not spilled) */
      if (sv->pr0 != PREG_NONE && !(sv->pr0 & PREG_SPILLED))
      {
        base = sv->pr0;
        v = VT_LOCAL; /* Set v to VT_LOCAL so the store is emitted below */
      }
      else
      {
        /* pr0 is spilled or invalid.
         *
         * There are two cases:
         * 1) Spilled pointer value (TEMP/PARAM): the spill slot holds an address.
         *    We must load that address and store through it.
         * 2) Concrete stack storage (VAR/local): sv->c.i is the stack offset of the object.
         *    We must store directly to [FP+offset] (no extra indirection).
         *
         * Misclassifying case (2) as (1) produces code like:
         *   ldr rA, [fp, #-off]; str rX, [rA]
         * which treats the object contents as a pointer (often uninitialized), corrupting
         * computations like 64-bit mul expansions.
         */
        int is_spilled_ptr = 0;
        if (sv->pr0 != PREG_NONE && (sv->pr0 & PREG_SPILLED) && (fr & VT_LVAL) &&
            tcc_is_vreg_valid(tcc_state->ir, sv->vr))
        {
          int vreg_type = TCCIR_DECODE_VREG_TYPE(sv->vr);
          is_spilled_ptr = (vreg_type == TCCIR_VREG_TYPE_TEMP || vreg_type == TCCIR_VREG_TYPE_PARAM);
        }

        if (is_spilled_ptr)
        {
          int addr_offset = sv->c.i;
          int addr_sign = (addr_offset < 0);
          if (addr_sign)
            addr_offset = -addr_offset;

          /* Load the address into a free scratch register.
           * Exclude the source register 'r' to avoid overwriting the value we want to store. */
          uint32_t exclude_regs = (1u << r);
          ScratchRegAlloc base_alloc = get_scratch_reg_with_save(exclude_regs);
          int base_reg = base_alloc.reg;

          if (!load_word_from_base(base_reg, R_FP, addr_offset, addr_sign))
          {
            ScratchRegAlloc rr_alloc = th_offset_to_reg(addr_offset, addr_sign);
            int rr = rr_alloc.reg;
            ot_check(th_ldr_reg(base_reg, R_FP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
            restore_scratch_reg(&rr_alloc);
          }
          base = base_reg;
          v = VT_LOCAL;
          fc = sign = 0;
          restore_scratch_reg(&base_alloc);
        }
        else
        {
          /* Treat as concrete stack storage at [FP + sv->c.i]. */
          base = R_FP;
          v = VT_LOCAL;
          /* Keep fc/sign as computed from sv->c.i above. */
        }
      }
    }
    else if (v == VT_LOCAL && sv->pr0 != PREG_NONE && (sv->pr0 & PREG_SPILLED) && (fr & VT_LVAL) &&
             tcc_is_vreg_valid(tcc_state->ir, sv->vr))
    {
      /* Spilled pointer - the address we want to store to is in the spill slot.
       * Load the address from stack, then store through it with offset 0.
       *
       * IMPORTANT: Only treat TEMP/PARAM vregs this way.
       * Regular locals (VAR vregs) spilled to stack represent concrete storage
       * and must be stored to directly at [FP+off], not indirectly via their
       * current contents. Misclassifying locals here leads to stores like
       *   ldr r0, [fp, #-4]; str rX, [r0]
       * which breaks simple loops (e.g. 118_switch.c never increments i).
       */
      int vreg_type = TCCIR_DECODE_VREG_TYPE(sv->vr);
      if (vreg_type != TCCIR_VREG_TYPE_TEMP && vreg_type != TCCIR_VREG_TYPE_PARAM)
      {
        /* Not a spilled pointer value; fall through to direct stack store. */
      }
      else
      {
        int addr_offset = sv->c.i;
        int addr_sign = (addr_offset < 0);
        if (addr_sign)
          addr_offset = -addr_offset;
        /* Load the address into a free scratch register.
         * Exclude the source register 'r' to avoid overwriting the value we want to store. */
        uint32_t exclude_regs = (1 << r);
        ScratchRegAlloc base_alloc = get_scratch_reg_with_save(exclude_regs);
        int base_reg = base_alloc.reg;

        if (!load_word_from_base(base_reg, R_FP, addr_offset, addr_sign))
        {
          ScratchRegAlloc rr_alloc = th_offset_to_reg(addr_offset, addr_sign);
          int rr = rr_alloc.reg;
          ot_check(th_ldr_reg(base_reg, R_FP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          restore_scratch_reg(&rr_alloc);
        }
        base = base_reg;
        /* Store to [base + 0] since the address already includes any offset */
        fc = sign = 0;
        restore_scratch_reg(&base_alloc);
      }
    }
    else if (v == VT_CONST)
    {
      /* Load the base address of the global symbol (without offset) */
      SValue v1;
      Sym *validated_sym = (sv->r & VT_SYM) ? validate_sym_for_reloc(sv->sym) : NULL;
      memset(&v1, 0, sizeof(SValue));
      v1.type.t = ft;
      v1.r = (fr & ~VT_LVAL) | (validated_sym ? VT_SYM : 0);
      v1.c.i = 0; /* Load base address, not base+offset */
      v1.sym = validated_sym;
      v1.pr0 = PREG_NONE; /* Mark as not having a preloaded register */

      /* Find a free scratch register for loading the global symbol address.
       * Exclude the source register 'r' to avoid overwriting the value we want to store. */
      uint32_t exclude_regs = (1 << r);
      ScratchRegAlloc base_alloc = get_scratch_reg_with_save(exclude_regs);
      base = base_alloc.reg;

      load(base, &v1);
      /* fc already has the field offset from sv->c.i */
      sign = 0;
      v = VT_LOCAL;
      restore_scratch_reg(&base_alloc);
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
            ot_check(th_vstr(base, r, !sign, 1, fc));
          else
            ot_check(th_vstr(base, r, !sign, 0, fc));
        }
        else
        {
          /* Source is integer register - use regular STR for soft float path */
          if ((ft & VT_BTYPE) == VT_FLOAT)
          {
            /* Single precision - one 32-bit store */
            if (!ot(th_str_imm(r, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE)))
            {
              ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(fc, sign, (1u << r) | (1u << base));
              int rr = rr_alloc.reg;
              ot_check(th_str_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
              restore_scratch_reg(&rr_alloc);
            }
          }
          else
          {
            /* Double precision - two 32-bit stores (low word first) */
            /* Use sv->pr1 for high register, not r+1 which could be invalid */
            int r_high = sv->pr1;
            if (r_high == PREG_NONE || r_high == R_SP || r_high == R_PC)
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
              ScratchRegAlloc rr_alloc_lo = th_offset_to_reg_ex(fc, sign, (1u << r) | (1u << base));
              int rr = rr_alloc_lo.reg;
              ot_check(th_str_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
              restore_scratch_reg(&rr_alloc_lo);
            }
            /* Store high word at fc+4 */
            if (!ot(th_str_imm(r_high, base, fc + 4, sign ? 4 : 6, ENFORCE_ENCODING_NONE)))
            {
              ScratchRegAlloc rr_alloc_hi = th_offset_to_reg_ex(fc + 4, sign, (1u << r_high) | (1u << base));
              int rr = rr_alloc_hi.reg;
              ot_check(th_str_reg(r_high, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
              restore_scratch_reg(&rr_alloc_hi);
            }
          }
        }
      }
      else if ((ft & VT_BTYPE) == VT_SHORT)
      {
        if (!ot(th_strh_imm(r, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE)))
        {
          ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(fc, sign, (1u << r) | (1u << base));
          int rr = rr_alloc.reg;
          ot_check(th_strh_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          restore_scratch_reg(&rr_alloc);
        }
      }
      else if ((ft & VT_BTYPE) == VT_BYTE)
      {
        if (!ot(th_strb_imm(r, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE)))
        {
          ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(fc, sign, (1u << r) | (1u << base));
          int rr = rr_alloc.reg;
          ot_check(th_strb_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          restore_scratch_reg(&rr_alloc);
        }
      }
      else
      {
        TRACE("store: sign: %x, r: %x, base: %x, fc: %x", sign, r, base, fc);
        if (!ot(th_str_imm(r, base, fc, sign ? 4 : 6, ENFORCE_ENCODING_NONE)))
        {
          ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(fc, sign, (1u << r) | (1u << base));
          int rr = rr_alloc.reg;
          ot_check(th_str_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          restore_scratch_reg(&rr_alloc);
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
    ot_check(th_vldr(base, r, !sign, 1, fc));
  }
  else
  {
    ot_check(th_vldr(base, r, !sign, 0, fc));
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
  ElfSym *esym = NULL;
  ThumbLiteralPoolEntry *entry;
  int sym_off = 0;
  thumb_opcode load_ins;
  int patch_pos;

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
  TRACE("'load_full_const' to register: %d, with imm: %d\n", r, imm);

  /* Emit the instruction first.
   * ot() may flush the current literal pool BEFORE emitting this op.
   * If patch_position is captured before ot_check(), it can end up pointing
   * at the pool skip-branch and later patching would clobber it.
   */
  if (r1 == PREG_NONE)
  {
    load_ins = th_ldr_literal(r, 0, 1);
  }
  else
  {
    load_ins = th_ldrd_imm(r, r1, R_PC, 0, 4, ENFORCE_ENCODING_NONE);
  }
  ot_check(load_ins);
  patch_pos = ind - load_ins.size;

  entry = th_literal_pool_find_or_allocate(sym, imm);
  entry->sym = sym;
  entry->imm = imm;
  entry->patch_position = patch_pos;
  entry->relocation = -1; /* No relocation by default */
  entry->data_size = (r1 == PREG_NONE) ? 4 : 8;
  entry->short_instruction = (r1 == PREG_NONE && load_ins.size == 2);

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
            /* Find a free scratch register for literal pool entry */
            uint32_t exclude_regs = (1 << r); /* Exclude destination register */
            ScratchRegAlloc scratch_alloc = get_scratch_reg_with_save(exclude_regs);
            int scratch = scratch_alloc.reg;

            thumb_opcode ldr = th_ldr_literal(scratch, 0, 1);
            ot_check(ldr);

            ThumbLiteralPoolEntry *entry2 = th_literal_pool_allocate();
            entry2->sym = NULL;
            entry2->imm = imm;
            entry2->patch_position = ind - ldr.size;
            entry2->relocation = -1;
            entry2->data_size = 4;
            entry2->short_instruction = (ldr.size == 2);
            ot_check(
                th_add_reg(r, r, scratch, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
            restore_scratch_reg(&scratch_alloc);
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
            /* Find a free scratch register for literal pool entry */
            uint32_t exclude_regs = (1 << r); /* Exclude destination register */
            ScratchRegAlloc scratch_alloc = get_scratch_reg_with_save(exclude_regs);
            int scratch = scratch_alloc.reg;

            thumb_opcode ldr = th_ldr_literal(scratch, 0, 1);
            ot_check(ldr);

            ThumbLiteralPoolEntry *entry2 = th_literal_pool_allocate();
            entry2->sym = NULL;
            entry2->imm = imm;
            entry2->patch_position = ind - ldr.size;
            entry2->relocation = -1;
            entry2->data_size = 4;
            entry2->short_instruction = (ldr.size == 2);
            ot_check(
                th_add_reg(r, r, scratch, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
            restore_scratch_reg(&scratch_alloc);
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

ST_FUNC void tcc_machine_addr_of_stack_slot(int dest_reg, int frame_offset)
{
  if (dest_reg == PREG_NONE)
    tcc_error("compiler_error: addr_of_stack_slot requires a destination register");

  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;

  if (frame_offset == 0)
  {
    if (dest_reg != base_reg)
    {
      ot_check(th_mov_reg(dest_reg, base_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                          false));
    }
    return;
  }

  const int neg = (frame_offset < 0);
  int abs_off = neg ? -frame_offset : frame_offset;
  thumb_opcode op = neg ? th_sub_imm(dest_reg, base_reg, abs_off, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)
                        : th_add_imm(dest_reg, base_reg, abs_off, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);

  if (op.size != 0)
  {
    ot_check(op);
    return;
  }

  ScratchRegAlloc offset_alloc = {0};
  int offset_reg = dest_reg;

  if (dest_reg == base_reg)
  {
    offset_alloc = get_scratch_reg_with_save(1u << base_reg);
    if (offset_alloc.reg == PREG_NONE)
      tcc_error("compiler_error: unable to allocate scratch register for stack address");
    offset_reg = offset_alloc.reg;
  }

  load_full_const(offset_reg, PREG_NONE, frame_offset, NULL);
  ot_check(th_add_reg(dest_reg, base_reg, offset_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                      ENFORCE_ENCODING_NONE));

  if (dest_reg == base_reg)
  {
    restore_scratch_reg(&offset_alloc);
  }
}

void load_vt_lval_vt_local(int r, int r1, SValue *sv, int ft, int fc, int sign, uint32_t base)
{
  int success = 0;
  const int btype = ft & VT_BTYPE;

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
      TRACE("load float to VFP r: %d, base: %d, fc: %d, sign: %d\n", r, base, fc, sign);
      return load_vt_lval_vt_local_float(r, sv, ft, fc, sign, base);
    }
    else
    {
      /* Integer register - load float as raw bits (soft float) */
      TRACE("load float to INT r: %d, base: %d, fc: %d, sign: %d\n", r, base, fc, sign);
      if (btype == VT_DOUBLE || btype == VT_LDOUBLE)
      {
        /* Double: load 64 bits to pre-allocated register pair.
         * Use r1 parameter (high register) from caller.
         * Fallback to ir+1 for backward compatibility if r1 is not provided (-1). */
        int ir_high = r1;
        if (ir_high < 0)
        {
          /* Fallback: try r+1 but validate */
          ir_high = r + 1;
          if (ir_high == R_SP || ir_high == R_PC)
          {
            tcc_error("compiler_error: cannot load double - no valid high register "
                      "(r1=%d, r+1=%d would be SP/PC)\n",
                      r1, r + 1);
          }
        }
        /* If base overlaps with destination, preserve it for the pair load.
         * Otherwise the first load clobbers the base before the second load.
         */
        ScratchRegAlloc base_alloc = {0};
        uint32_t base_reg = base;
        if (base_reg == (uint32_t)r || base_reg == (uint32_t)ir_high)
        {
          uint32_t exclude = (1u << r) | (1u << ir_high);
          base_alloc = get_scratch_reg_with_save(exclude);
          base_reg = (uint32_t)base_alloc.reg;
          ot_check(th_mov_reg((int)base_reg, (int)base, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE, false));
        }

        /* Load low word first */
        success = load_word_from_base(r, base_reg, fc, sign);
        if (!success)
        {
          ScratchRegAlloc rr_alloc =
              th_offset_to_reg_ex(fc, sign, (1u << r) | (1u << base_reg) | (ir_high >= 0 ? (1u << ir_high) : 0));
          int rr = rr_alloc.reg;
          ot_check(th_ldr_reg(r, base_reg, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          restore_scratch_reg(&rr_alloc);
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
        success = load_word_from_base(ir_high, base_reg, fc_high, sign_high);
        if (!success)
        {
          ScratchRegAlloc rr_alloc =
              th_offset_to_reg_ex(fc_high, sign_high, (1u << ir_high) | (1u << base_reg) | (r >= 0 ? (1u << r) : 0));
          int rr = rr_alloc.reg;
          ot_check(th_ldr_reg(ir_high, base_reg, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          restore_scratch_reg(&rr_alloc);
        }

        if (base_alloc.saved)
        {
          restore_scratch_reg(&base_alloc);
        }
      }
      else
      {
        /* Float: load 32 bits to single integer register */
        success = load_word_from_base(r, base, fc, sign);
        if (!success)
        {
          ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(fc, sign, (1u << r) | (1u << base));
          int rr = rr_alloc.reg;
          ot_check(th_ldr_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          restore_scratch_reg(&rr_alloc);
        }
      }
      return;
    }
  }
  else if (btype == VT_LLONG || btype == VT_PTR)
  {
    /* 64-bit integer type - load to register pair if r1 is provided and valid */
    /* Note: r1 comes from uint8_t pr1, so -1 becomes 255. Check r1 <= 15 to exclude invalid values. */
    if (r1 >= 0 && r1 <= 15 && r1 != R_SP && r1 != R_PC && (btype == VT_LLONG))
    {
      TRACE("load 64-bit int to r:%d:r1:%d, base: %d, fc: %d, sign: %d\n", r, r1, base, fc, sign);
      /* If base overlaps with destination, preserve it for the pair load.
       * Otherwise the first load clobbers the base before the second load.
       */
      ScratchRegAlloc base_alloc = {0};
      uint32_t base_reg = base;
      if (base_reg == (uint32_t)r || base_reg == (uint32_t)r1)
      {
        uint32_t exclude = (1u << r) | (1u << r1);
        base_alloc = get_scratch_reg_with_save(exclude);
        base_reg = (uint32_t)base_alloc.reg;
        ot_check(th_mov_reg((int)base_reg, (int)base, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE, false));
      }
      /* Load low word */
      success = load_word_from_base(r, base_reg, fc, sign);
      if (!success)
      {
        ScratchRegAlloc rr_alloc =
            th_offset_to_reg_ex(fc, sign, (1u << r) | (1u << base_reg) | (r1 >= 0 ? (1u << r1) : 0));
        int rr = rr_alloc.reg;
        ot_check(th_ldr_reg(r, base_reg, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&rr_alloc);
      }
      /* Load high word at offset+4 */
      int fc_high = sign ? (fc - 4) : (fc + 4);
      int sign_high = sign;
      if (sign && fc_high < 0)
      {
        fc_high = -fc_high;
        sign_high = 0;
      }
      success = load_word_from_base(r1, base_reg, fc_high, sign_high);
      if (!success)
      {
        ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(fc_high, sign_high, (1u << r1) | (1u << base_reg) | (1u << r));
        int rr = rr_alloc.reg;
        ot_check(th_ldr_reg(r1, base_reg, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&rr_alloc);
      }

      if (base_alloc.saved)
      {
        restore_scratch_reg(&base_alloc);
      }
      return;
    }
    /* Fall through to 32-bit load for pointers or if r1 not provided */
  }
  else if (btype == VT_SHORT)
  {
    TRACE("load short to r: %d, base: %d, fc: %d, sign: %d\n", r, base, fc, sign);
    if (!(ft & VT_UNSIGNED))
    {
      success = load_short_from_base(r, base, fc, sign);
    }
    else
    {
      success = load_ushort_from_base(r, base, fc, sign);
    }
  }
  else if (btype == VT_BYTE || btype == VT_BOOL)
  {
    if (!(ft & VT_UNSIGNED))
    {
      success = load_byte_from_base(r, base, fc, sign);
    }
    else
    {
      success = load_ubyte_from_base(r, base, fc, sign);
    }
  }
  else
  {
    success = load_word_from_base(r, base, fc, sign);
  }
  if (!success)
  {

    // now load from dereferenced value
    ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(fc, sign, (1u << r) | (1u << base));
    int rr = rr_alloc.reg;
    if (btype == VT_SHORT)
    {
      if (ft & VT_UNSIGNED)
        ot_check(th_ldrh_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      else
        ot_check(th_ldrsh_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    else if (btype == VT_BYTE || btype == VT_BOOL)
    {
      if (ft & VT_UNSIGNED)
        ot_check(th_ldrb_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      else
        ot_check(th_ldrsb_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    else
      ot_check(th_ldr_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));

    restore_scratch_reg(&rr_alloc);
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
      load_full_const(r, PREG_NONE, lo, 0);
    if (!ot(o2))
      load_full_const(r1, PREG_NONE, hi, 0);
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

void load_vt_local(int r, SValue *sv, int base)
{
  int off = sv->c.i;
  /* Stack parameters live above the saved-register area.
   * When computing their address, fold in offset_to_args (prologue push size).
   */
  if (sv->r & VT_PARAM)
    off += offset_to_args;

  TRACE("'load_vt_local' r: %d, off: %x", r, (uint32_t)off);
  Sym *sym_to_use = NULL;
  if (sv->r & VT_SYM)
  {
    sym_to_use = validate_sym_for_reloc(sv->sym);
  }
  if (sym_to_use || (-off) >= 0xfff)
  {
    load_full_const(r, PREG_NONE, off, sym_to_use);
    ot_check(th_add_reg(r, base, r, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
  }
  else
  {
    ot_check(th_sub_imm(r, base, -off, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }
}

void load_vt_cmp(int r, SValue *sv)
{
  const uint32_t firstcond = mapcc(sv->c.i);
  TRACE("'load_vt_cmp' to reg: %d, op: 0x%x\n", r, (uint32_t)sv->c.i);
  if (r == R_SP || r == R_PC)
  {
    tcc_error("compiler_error: load_vt_cmp can't be used for pc or sp\n");
  }

  // it block
  o(0xbf00 | (firstcond << 4) | 0x4 | ((~firstcond & 1) << 3));
  ot_check(th_generic_mov_imm(r, 1));
  ot_check(th_generic_mov_imm(r, 0));
}

void load_vt_jmp_jmpi(int r, SValue *sv)
{
#ifdef TCC_TARGET_ARM_ARCHV6M
  if (r > 7)
  {
    tcc_error("compiler_error: implement load_vt_jmp_jmpi for armv6m\n");
  }
#endif
  ot_check(th_generic_mov_imm(r, sv->r & 1));
  ot_check(th_b_t4(2));
  gsym(sv->c.i);
  ot_check(th_generic_mov_imm(r, (sv->r ^ 1) & 1));
}

void load_to_dest(SValue *dest, SValue *sv)
{
  int v, ft, fr, sign;
  int64_t fc;
  fr = sv->r;
  ft = sv->type.t;
  fc = sv->c.i;
  int btype = ft & VT_BTYPE;

  fprintf(stderr,
          "[LOAD_TO_DEST] dest.pr0=%d dest.pr1=%d sv.vr=%d sv.r=0x%x sv.pr0=0x%x sv.c.i=%ld VT_LVAL=%d VT_PARAM=%d "
          "VT_LOCAL=%d\n",
          dest->pr0, dest->pr1, sv->vr, sv->r, sv->pr0, (long)sv->c.i, (sv->r & VT_LVAL) ? 1 : 0,
          (sv->r & VT_PARAM) ? 1 : 0, (sv->r & VT_VALMASK) == VT_LOCAL ? 1 : 0);

  /* If we're about to write into the register currently used to cache a global
   * symbol base address, invalidate the cache first. Otherwise the cache can
   * become stale (same register, different contents) and later loads may
   * incorrectly reuse it (e.g. clobbering stdout setup when loading a literal). */
  if (thumb_gen_state.cached_global_reg != PREG_NONE &&
      (dest->pr0 == thumb_gen_state.cached_global_reg || dest->pr1 == thumb_gen_state.cached_global_reg))
  {
    thumb_gen_state.cached_global_sym = NULL;
    thumb_gen_state.cached_global_reg = PREG_NONE;
  }

  /* Handle invalid/uninitialized SValue: if the value part (VT_VALMASK) is 0x3f,
   * which is an invalid register/value code, this is likely corrupted or
   * uninitialized. Just load 0 as a fallback. */
  if ((fr & VT_VALMASK) == 0x3f)
  {
    /* Load zero as a safe default */
    ot_check(th_mov_imm(dest->pr0, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    if (dest->pr1 != PREG_NONE)
      ot_check(th_mov_imm(dest->pr1, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    return;
  }

  if (fc >= 0)
    sign = 0;
  else
  {
    sign = 1;
    fc = -fc;
  }

  /* Parameters passed on the stack are always accessed via FP with positive offsets.
   * offset_to_args is only for computing FP-relative offsets, not SP-relative.
   * The ARM EABI places stack parameters in the caller's frame above the saved FP. */
  if (sv->r & VT_PARAM)
  {
    fc += offset_to_args;
  }

  v = fr & VT_VALMASK;

  // load lvalue from
  if (fr & VT_LVAL)
  {
    /* When we don't keep a frame pointer, all stack addressing must be SP-relative.
     * For stack parameters we already fold in `offset_to_args`, so SP-relative
     * addressing still reaches the caller-argument area correctly.
     */
    uint32_t base = tcc_state->need_frame_pointer ? R_FP : R_SP;
    SValue v1;

    // /* First check if this is a register-allocated LOCAL variable.
    //  * In this case pr0 contains the allocated register, and we should
    //  * do a register move instead of loading from memory.
    //  * NOTE: This only applies to VT_LOCAL, NOT to v < VT_CONST which means
    //  * the address is in a register and needs dereferencing. */
    // if (v == VT_LOCAL && sv->pr0 >= 0 && !(sv->pr0 & PREG_SPILLED))
    // {
    //   /* Allocated to register - do register move, not memory load. */
    //   /* For doubles in integer registers (soft float) */
    //   if (dest->pr0 != sv->pr0)
    //   {
    //     ot_check(th_mov_reg(dest->pr0, sv->pr0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
    //                         ENFORCE_ENCODING_NONE, false));
    //   }
    //   if (tcc_is_64bit_operand(dest) && dest->pr1 != sv->pr1)
    //   {
    //     ot_check(th_mov_reg(dest->pr1, sv->pr1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
    //                         ENFORCE_ENCODING_NONE, false));
    //   }
    //   return;
    // }

    // load value from stack
    // prepare for new load after pointer dereference
    if (v == VT_LLOCAL)
    {
      v1.type.t = VT_PTR;
      v1.r = VT_LOCAL | VT_LVAL;
      /* For VT_LLOCAL parameters, the pointer is stored at the parameter location.
       * sv->c.i contains the base parameter offset (e.g., 0 for first param).
       * For parameters, we need to add offset_to_args to get the FP-relative offset. */
      v1.c.i = sv->c.i;

      if (sv->r & VT_PARAM)
      {
        v1.c.i += offset_to_args;
      }

      ScratchRegAlloc base_alloc = get_scratch_reg_with_save(0);
      base = base_alloc.reg;
      load(base, &v1);
      restore_scratch_reg(&base_alloc);
      fc = sign = 0;
      v = VT_LOCAL;
    }
    else if (v == VT_CONST)
    {
      Sym *validated_sym = (sv->r & VT_SYM) ? validate_sym_for_reloc(sv->sym) : NULL;
      memset(&v1, 0, sizeof(SValue));
      v1.type.t = VT_PTR;
      v1.r = (fr & ~VT_LVAL) | (validated_sym ? VT_SYM : 0);
      v1.c.i = 0; /* Load base address, not base+offset */
      v1.sym = validated_sym;
      v1.pr0 = PREG_NONE; /* Mark as not having a preloaded register */
      TRACE("l2");
      ScratchRegAlloc base_alloc = get_scratch_reg_with_save(0);
      base = base_alloc.reg;
      load(base, &v1);
      restore_scratch_reg(&base_alloc);
      /* fc already has the field offset from sv->c.i */
      sign = 0;
      v = VT_LOCAL;
    }
    else if (v < VT_CONST)
    {
      /* For spilled lvalues, we need two-level indirection:
       * 1. Load the pointer from spill location [FP + spill_offset]
       * 2. Dereference that pointer to get the final value
       * For non-spilled, the pointer is already in a register (pr0). */
      if (sv->pr0 != PREG_NONE && (sv->pr0 & PREG_SPILLED))
      {
        SValue v1;
        memset(&v1, 0, sizeof(SValue));
        v1.type.t = VT_PTR;
        v1.r = VT_LOCAL | VT_LVAL;
        v1.c.i = sv->c.i;
        v1.pr0 = PREG_NONE; /* Mark as not having a preloaded register */

        TRACE("load_to_dest: loading spilled lvalue address from [FP%+lld]", (long long)fc);
        ScratchRegAlloc base_alloc = get_scratch_reg_with_save(0);
        base = base_alloc.reg;
        load(base, &v1); /* Load pointer into free scratch register first */
        restore_scratch_reg(&base_alloc);
        fc = sign = 0; /* Dereference with offset 0 from loaded pointer */
        v = VT_LOCAL;
      }
      else
      {
        base = sv->pr0;
        fc = sign = 0;
        v = VT_LOCAL;
      }
    }
    else if (v == VT_LOCAL && sv->pr0 != PREG_NONE && (sv->pr0 & PREG_SPILLED))
    {
      /* Spilled lvalue case: The pointer is spilled to stack at sv->c.i.
       * We need two-level indirection:
       * 1. Load the pointer from [FP + sv->c.i]
       * 2. Dereference that pointer to get the final value
       */
      SValue v1;
      memset(&v1, 0, sizeof(SValue));
      v1.type.t = VT_PTR;
      v1.r = VT_LOCAL | VT_LVAL;
      v1.c.i = sv->c.i;
      v1.pr0 = PREG_NONE; /* Mark as not having a preloaded register */

      ScratchRegAlloc base_alloc = get_scratch_reg_with_save(0);
      base = base_alloc.reg;
      load(base, &v1); /* Load pointer into scratch register */
      restore_scratch_reg(&base_alloc);
      fc = sign = 0; /* Dereference with offset 0 from loaded pointer */
      /* v remains VT_LOCAL so we fall through to load_vt_lval_vt_local */
    }
    else if (v == VT_LOCAL && sv->pr0 != PREG_NONE && !(sv->pr0 & PREG_SPILLED))
    {
      /* Preloaded pointer case: pr0 contains the address to load from.
       * This happens when tcc_ir_preload_spills loaded a spilled pointer. */
      base = sv->pr0;
      fc = sign = 0;
    }

    if (v == VT_LOCAL)
    {
      /* Invalidate global symbol cache if we're writing to the cached register */
      if (dest->pr0 == thumb_gen_state.cached_global_reg || dest->pr1 == thumb_gen_state.cached_global_reg)
      {
        thumb_gen_state.cached_global_sym = NULL;
        thumb_gen_state.cached_global_reg = PREG_NONE;
      }
      return load_vt_lval_vt_local(dest->pr0, dest->pr1, sv, ft, fc, sign, base);
    }
  }
  else if (v == VT_CONST)
    return load_vt_const(dest->pr0, dest->pr1, sv);
  else if (v == VT_LOCAL)
  {
    /* Check if this is a spilled value that needs loading vs address-of computation.
     * - Spilled value load: pr0 has PREG_SPILLED bit, VT_LVAL was stripped during preload
     *   -> we need to load the VALUE from stack
     * - Address-of spilled vreg: pr0 has PREG_SPILLED bit, never had VT_LVAL
     *   -> we need to compute the ADDRESS (FP + offset)
     * - Address-of non-spilled: pr0 == PREG_NONE
     *   -> compute address
     *
     * Since we can't distinguish based on VT_LVAL (both cases have no VT_LVAL at this point),
     * we check if this is coming from a preloaded path vs direct.
     * If pr0 has PREG_SPILLED and c.i != 0, AND we don't have a valid vr, then load value.
     * If pr0 has PREG_SPILLED but we have a valid vr (address-of vreg), compute address.
     */
    if (sv->pr0 != PREG_NONE && (sv->pr0 & PREG_SPILLED))
    {
      /* VT_LOCAL without VT_LVAL is ambiguous in this backend:
       * - real locals/vars: treat as address-of (compute FP + offset)
       * - spilled values (TMPs, often also spilled PARAMs): treat as value-in-slot (LDR)
       *
       * Using the address for spilled TMPs breaks arithmetic, e.g. in ir_tests/20_op_add
       * simple5() would compute x * (&y_slot) instead of x * y.
       */

      int vreg_type = (sv->vr == -1) ? 0 : TCCIR_DECODE_VREG_TYPE(sv->vr);
      /* NOTE: `vr == 0` is used by this IR backend for some real stack locals.
       * Treating it as an invalid vreg here causes us to LDR the slot value when the
       * caller actually needs the slot *address* (e.g. for storing back a local loop
       * counter), which can lead to stores to address `i` instead of [FP+off]. */
      const int is_spilled_value =
          (sv->vr == -1 || vreg_type == TCCIR_VREG_TYPE_TEMP || vreg_type == TCCIR_VREG_TYPE_PARAM);
      if (is_spilled_value)
      {
        int src_offset = sv->c.i;
        int src_sign = (src_offset < 0);
        int src_abs = src_sign ? -src_offset : src_offset;
        if (!load_word_from_base(dest->pr0, R_FP, src_abs, src_sign))
        {
          ScratchRegAlloc rr_alloc = th_offset_to_reg(src_abs, src_sign);
          int rr = rr_alloc.reg;
          ot_check(th_ldr_reg(dest->pr0, R_FP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          restore_scratch_reg(&rr_alloc);
        }
        return;
      }
      /* Otherwise: address-of local storage; fall through to compute address. */
    }
    /* Either pr0 == PREG_NONE (address computation) or address-of with PREG_SPILLED.
     * Compute address using load_vt_local. */
    int base = R_FP;
    if (tcc_state->need_frame_pointer == 0)
    {
      base = R_SP;
    }
    return load_vt_local(dest->pr0, sv, base);
  }
  else if (v == VT_CMP)
    return load_vt_cmp(dest->pr0, sv);
  else if (v == VT_JMP || v == VT_JMPI)
    return load_vt_jmp_jmpi(dest->pr0, sv);
  else if (v < VT_CONST)
  {
    /* For IR-generated code, use pr0 as the source register */
    int src_reg = (sv->pr0 != PREG_NONE && !(sv->pr0 & PREG_SPILLED)) ? sv->pr0 : v;

    /* Check if spilled - load from stack instead of register move.
     * Note: pr0 might have been overwritten with destination register by caller,
     * so also check if c.i is non-zero (stack offset) as a backup indicator. */
    if (((sv->pr0 != PREG_NONE) && (sv->pr0 & PREG_SPILLED)) ||
        (v < VT_CONST && sv->c.i != 0 && (sv->r & VT_LVAL) == 0))
    {
      /* Value is spilled to stack at sv->c.i offset from FP */
      int src_offset = sv->c.i;
      int src_sign = (src_offset < 0);
      int src_abs = src_sign ? -src_offset : src_offset;
      if (!load_word_from_base(dest->pr0, R_FP, src_abs, src_sign))
      {
        ScratchRegAlloc rr_alloc = th_offset_to_reg(src_abs, src_sign);
        int rr = rr_alloc.reg;
        ot_check(th_ldr_reg(dest->pr0, R_FP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&rr_alloc);
      }
      return;
    }

    if (is_float(ft))
    {
      /* Check if we're moving between VFP registers or integer registers.
       * Only use VFP if hard float ABI is enabled. */
      if (tcc_state->float_abi == ARM_HARD_FLOAT && dest->pr0 != TREG_F0 && dest->pr0 <= TREG_F7 &&
          src_reg >= TREG_F0 && src_reg <= TREG_F7)
      {
        /* VFP to VFP move */
        if ((ft & VT_BTYPE) == VT_FLOAT)
          ot_check(th_vmov_register(dest->pr0, src_reg, 0));
        else
          ot_check(th_vmov_register(dest->pr0, src_reg, 1));
      }
      else
      {
        /* Integer register move (soft float) */
        if (dest->pr0 != src_reg)
        {
          ot_check(th_mov_reg(dest->pr0, src_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE, false));
        }
        if ((ft & VT_BTYPE) == VT_DOUBLE || (ft & VT_BTYPE) == VT_LDOUBLE)
        {
          /* Also move high word for double.
           * Use dest->pr1 for destination high register.
           * Source high register comes from sv->r2 if available, otherwise src_reg+1. */
          if (dest->pr1 != PREG_NONE)
          {
            int v_high = (sv->r2 != VT_CONST) ? sv->r2 : (src_reg + 1);
            if (dest->pr1 != v_high)
            {
              ot_check(th_mov_reg(dest->pr1, v_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                  ENFORCE_ENCODING_NONE, false));
            }
          }
        }
      }
    }
    else
    {
      /* Non-float register move */
      if (dest->pr0 != src_reg)
      {
        ot_check(th_mov_reg(dest->pr0, src_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE, false));
      }
      if (dest->pr1 != PREG_NONE && tcc_is_64bit_operand(sv))
      {
        int v_high = (sv->r2 != VT_CONST) ? sv->r2 : (src_reg + 1);
        if (dest->pr1 != v_high)
        {
          ot_check(th_mov_reg(dest->pr1, v_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE, false));
        }
      }
    }
    return;
  }
  tcc_error("compiler_error: unknown load not implemented: v=%d, fr=0x%x, ft=0x%x, VT_LVAL=%d\n", v, fr, ft,
            (fr & VT_LVAL) ? 1 : 0);
}

/* Wrapper for legacy load() calls - creates temporary dest SValue */
static void load_to_reg(int r, int r1, SValue *sv)
{
  SValue dest;
  memset(&dest, 0, sizeof(dest));
  dest.pr0 = r;
  dest.pr1 = r1; /* PREG_NONE for 32-bit, actual register for 64-bit */
  dest.type = sv->type;
  load_to_dest(&dest, sv);
}

// Simplified load() - now just calls load_to_reg()
void load(int r, SValue *sv)
{
  load_to_reg(r, PREG_NONE, sv);
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
      const int src2_is_imm = th_has_immediate_value(op->src2.r) || op->src2.pr0 == PREG_NONE;
      const uint64_t src2_imm = (uint64_t)op->src2.c.i;
      const uint32_t imm_low = (uint32_t)(src2_imm & 0xffffffffu);
      const uint32_t imm_high = (uint32_t)(src2_imm >> 32);

      /* Materialize spilled/unallocated regs (backend must never pass sentinels to opcode encoders). */
      ScratchRegAlloc rd0_alloc = {0};
      ScratchRegAlloc rn0_alloc = {0};
      ScratchRegAlloc rm0_alloc = {0};
      ScratchRegAlloc rd1_alloc = {0};
      ScratchRegAlloc rn1_alloc = {0};
      ScratchRegAlloc rm1_alloc = {0};

      const int rd0_is_mem = (op->dest.pr0 == PREG_NONE || op->dest.pr0 == PREG_SPILLED);
      const int rn0_is_mem = (op->src1.pr0 == PREG_NONE || op->src1.pr0 == PREG_SPILLED);
      const int rm0_is_mem = (!src2_is_imm && (op->src2.pr0 == PREG_NONE || op->src2.pr0 == PREG_SPILLED));

      int rd0 = op->dest.pr0;
      int rn0 = op->src1.pr0;
      int rm0 = op->src2.pr0;

      uint32_t exclude0 = (1u << R_SP);
      if (!rd0_is_mem && rd0 >= 0 && rd0 <= 15)
        exclude0 |= (1u << rd0);
      if (!rn0_is_mem && rn0 >= 0 && rn0 <= 15)
        exclude0 |= (1u << rn0);
      if (!rm0_is_mem && !src2_is_imm && rm0 >= 0 && rm0 <= 15)
        exclude0 |= (1u << rm0);

      /* Check if operands are 64-bit memory values */
      const int rd_is_mem = rd0_is_mem || (op->dest.pr1 == PREG_NONE || op->dest.pr1 == PREG_SPILLED);
      const int rn_is_mem = rn0_is_mem || (op->src1.pr1 == PREG_NONE || op->src1.pr1 == PREG_SPILLED);
      const int rm_is_mem = rm0_is_mem || (!src2_is_imm && (op->src2.pr1 == PREG_NONE || op->src2.pr1 == PREG_SPILLED));

      /* For 64-bit memory operands, allocate BOTH registers together */
      int rd1 = op->dest.pr1;
      int rn1 = op->src1.pr1;
      int rm1 = op->src2.pr1;

      if (rd_is_mem && rd1 != PREG_NONE)
      {
        /* 64-bit dest in memory - allocate both registers */
        rd0_alloc = get_scratch_reg_with_save(exclude0);
        rd0 = rd0_alloc.reg;
        exclude0 |= (1u << rd0);
        rd1_alloc = get_scratch_reg_with_save(exclude0);
        rd1 = rd1_alloc.reg;
        exclude0 |= (1u << rd1);
        /* Only needed for ADC if dest is also src (carry propagation); safe to preload. */
        load_to_reg(rd0, rd1, &op->dest);
      }
      else if (rd0_is_mem)
      {
        /* 32-bit dest in memory */
        rd0_alloc = get_scratch_reg_with_save(exclude0);
        rd0 = rd0_alloc.reg;
        exclude0 |= (1u << rd0);
        load_to_reg(rd0, PREG_NONE, &op->dest);
      }

      if (rn_is_mem && rn1 != PREG_NONE)
      {
        /* 64-bit src1 in memory - allocate both registers */
        rn0_alloc = get_scratch_reg_with_save(exclude0);
        rn0 = rn0_alloc.reg;
        exclude0 |= (1u << rn0);
        rn1_alloc = get_scratch_reg_with_save(exclude0);
        rn1 = rn1_alloc.reg;
        exclude0 |= (1u << rn1);
        load_to_reg(rn0, rn1, &op->src1);
      }
      else if (rn0_is_mem)
      {
        /* 32-bit src1 in memory */
        rn0_alloc = get_scratch_reg_with_save(exclude0);
        rn0 = rn0_alloc.reg;
        exclude0 |= (1u << rn0);
        load_to_reg(rn0, PREG_NONE, &op->src1);
      }

      if (rm_is_mem && rm1 != PREG_NONE)
      {
        /* 64-bit src2 in memory - allocate both registers */
        rm0_alloc = get_scratch_reg_with_save(exclude0);
        rm0 = rm0_alloc.reg;
        exclude0 |= (1u << rm0);
        rm1_alloc = get_scratch_reg_with_save(exclude0);
        rm1 = rm1_alloc.reg;
        exclude0 |= (1u << rm1);
        load_to_reg(rm0, rm1, &op->src2);
      }
      else if (rm0_is_mem)
      {
        /* 32-bit src2 in memory */
        rm0_alloc = get_scratch_reg_with_save(exclude0);
        rm0 = rm0_alloc.reg;
        exclude0 |= (1u << rm0);
        load_to_reg(rm0, PREG_NONE, &op->src2);
      }

      /* 64-bit add: ADDS for low words, ADC for high words */
      /* dest.pr0:pr1 = src1.pr0:pr1 + src2.pr0:pr1 */
      if (src2_is_imm)
      {
        thumb_opcode add_low = th_add_imm(rd0, rn0, imm_low, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
        if (add_low.size == 0)
        {
          ScratchRegAlloc scratch = {0};
          uint32_t exclude = (1u << rd0) | (1u << rn0);
          scratch = get_scratch_reg_with_save(exclude);
          {
            SValue imm_sv = {0};
            imm_sv.r = VT_CONST;
            imm_sv.type.t = VT_INT;
            imm_sv.c.i = imm_low;
            load_vt_const(scratch.reg, PREG_NONE, &imm_sv);
          }
          ot_check(th_add_reg(rd0, rn0, scratch.reg, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          restore_scratch_reg(&scratch);
        }
        else
        {
          ot_check(add_low);
        }
      }
      else
      {
        ot_check(th_add_reg(rd0, rn0, rm0, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      }
      /* High word: handle mixed 32/64-bit operands (pr1 may be PREG_NONE). */
      if (src2_is_imm)
      {
        /* src2 high word comes from immediate */
        if (rn1 != PREG_NONE)
        {
          ot_check(th_adc_imm(rd1, rn1, imm_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        }
        else
        {
          ot_check(th_mov_imm(rd1, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
          ot_check(th_adc_imm(rd1, rd1, imm_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        }
      }
      else if (rn1 != PREG_NONE && rm1 != PREG_NONE)
      {
        ot_check(th_adc_reg(rd1, rn1, rm1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      }
      else if (rn1 != PREG_NONE)
      {
        /* src2 high word is 0 */
        ot_check(th_adc_imm(rd1, rn1, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      }
      else if (rm1 != PREG_NONE)
      {
        /* src1 high word is 0 */
        ot_check(th_mov_imm(rd1, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        ot_check(th_adc_reg(rd1, rd1, rm1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      }
      else
      {
        /* Both high words are 0, result is carry from low add */
        ot_check(th_mov_imm(rd1, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        ot_check(th_adc_imm(rd1, rd1, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      }

      /* Cleanup: restore scratch registers in reverse order */
      if (rd_is_mem && rd1 != PREG_NONE)
      {
        /* 64-bit dest - store both words manually */
        SValue dest_with_regs = op->dest;
        dest_with_regs.pr0 = rd0;
        dest_with_regs.pr1 = rd1;
        store(rd0, &dest_with_regs);
        /* Also need to store high word - create a modified SValue for high word */
        SValue dest_high = dest_with_regs;
        dest_high.c.i += 4; /* Offset for high word */
        store(rd1, &dest_high);
        restore_scratch_reg(&rd1_alloc);
        restore_scratch_reg(&rd0_alloc);
      }
      else if (rd0_is_mem)
      {
        /* 32-bit dest */
        store(rd0, &op->dest);
        restore_scratch_reg(&rd0_alloc);
      }

      if (rn_is_mem && rn1 != PREG_NONE)
      {
        restore_scratch_reg(&rn1_alloc);
        restore_scratch_reg(&rn0_alloc);
      }
      else if (rn0_is_mem)
      {
        restore_scratch_reg(&rn0_alloc);
      }

      if (rm_is_mem && rm1 != PREG_NONE)
      {
        restore_scratch_reg(&rm1_alloc);
        restore_scratch_reg(&rm0_alloc);
      }
      else if (rm0_is_mem)
      {
        restore_scratch_reg(&rm0_alloc);
      }

      return;
    }
    handler.imm_handler = th_add_imm;
    handler.reg_handler = th_add_reg;
    break;
  case TCCIR_OP_SUB:
    if (is_64bit)
    {
      const int src2_is_imm = th_has_immediate_value(op->src2.r) || op->src2.pr0 == PREG_NONE;
      const uint64_t src2_imm = (uint64_t)op->src2.c.i;
      const uint32_t imm_low = (uint32_t)(src2_imm & 0xffffffffu);
      const uint32_t imm_high = (uint32_t)(src2_imm >> 32);

      /* Handle memory operands for 64-bit subtraction */
      int rd0 = op->dest.pr0;
      int rn0 = op->src1.pr0;
      int rm0 = op->src2.pr0;
      int rd1 = op->dest.pr1;
      int rn1 = op->src1.pr1;
      int rm1 = op->src2.pr1;

      const int rd_is_mem = (op->dest.pr0 == PREG_NONE || op->dest.pr0 == PREG_SPILLED);
      const int rn_is_mem = (op->src1.pr0 == PREG_NONE || op->src1.pr0 == PREG_SPILLED);
      const int rm_is_mem = (!src2_is_imm && (op->src2.pr0 == PREG_NONE || op->src2.pr0 == PREG_SPILLED));

      ScratchRegAlloc rd_alloc = {0}, rn_alloc = {0}, rm_alloc = {0};
      ScratchRegAlloc rd1_alloc = {0}, rn1_alloc = {0}, rm1_alloc = {0};

      uint32_t exclude = 0;
      if (!rd_is_mem && rd0 >= 0 && rd0 <= 15)
        exclude |= (1u << rd0);
      if (!rd_is_mem && rd1 >= 0 && rd1 <= 15)
        exclude |= (1u << rd1);
      if (!rn_is_mem && rn0 >= 0 && rn0 <= 15)
        exclude |= (1u << rn0);
      if (!rn_is_mem && rn1 >= 0 && rn1 <= 15)
        exclude |= (1u << rn1);
      if (!rm_is_mem && !src2_is_imm && rm0 >= 0 && rm0 <= 15)
        exclude |= (1u << rm0);
      if (!rm_is_mem && !src2_is_imm && rm1 >= 0 && rm1 <= 15)
        exclude |= (1u << rm1);

      /* Load 64-bit values from memory */
      if (rd_is_mem)
      {
        rd_alloc = get_scratch_reg_with_save(exclude);
        rd0 = rd_alloc.reg;
        exclude |= (1u << rd0);
        rd1_alloc = get_scratch_reg_with_save(exclude);
        rd1 = rd1_alloc.reg;
        exclude |= (1u << rd1);
        load_to_reg(rd0, rd1, &op->dest);
      }
      if (rn_is_mem)
      {
        rn_alloc = get_scratch_reg_with_save(exclude);
        rn0 = rn_alloc.reg;
        exclude |= (1u << rn0);
        rn1_alloc = get_scratch_reg_with_save(exclude);
        rn1 = rn1_alloc.reg;
        exclude |= (1u << rn1);
        load_to_reg(rn0, rn1, &op->src1);
      }
      if (rm_is_mem)
      {
        rm_alloc = get_scratch_reg_with_save(exclude);
        rm0 = rm_alloc.reg;
        exclude |= (1u << rm0);
        rm1_alloc = get_scratch_reg_with_save(exclude);
        rm1 = rm1_alloc.reg;
        exclude |= (1u << rm1);
        load_to_reg(rm0, rm1, &op->src2);
      }

      /* 64-bit sub: SUBS for low words, SBC for high words */
      if (src2_is_imm)
      {
        thumb_opcode sub_low = th_sub_imm(rd0, rn0, imm_low, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
        if (sub_low.size == 0)
        {
          ScratchRegAlloc scratch = {0};
          uint32_t exclude = (1u << rd0) | (1u << rn0);
          scratch = get_scratch_reg_with_save(exclude);
          {
            SValue imm_sv = {0};
            imm_sv.r = VT_CONST;
            imm_sv.type.t = VT_INT;
            imm_sv.c.i = imm_low;
            load_vt_const(scratch.reg, PREG_NONE, &imm_sv);
          }
          ot_check(th_sub_reg(rd0, rn0, scratch.reg, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          restore_scratch_reg(&scratch);
        }
        else
        {
          ot_check(sub_low);
        }
      }
      else
      {
        ot_check(th_sub_reg(rd0, rn0, rm0, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      }
      /* High word: handle mixed 32/64-bit operands (pr1 may be PREG_NONE). */
      if (src2_is_imm)
      {
        /* src2 high word comes from immediate */
        if (rn1 != PREG_NONE)
        {
          ot_check(th_sbc_imm(rd1, rn1, imm_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        }
        else
        {
          /* src1 high word is 0 (32-bit value promoted to 64-bit) */
          ot_check(th_mov_imm(rd1, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
          ot_check(th_sbc_imm(rd1, rd1, imm_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        }
      }
      else if (rn1 != PREG_NONE && rm1 != PREG_NONE)
      {
        ot_check(th_sbc_reg(rd1, rn1, rm1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      }
      else if (rn1 != PREG_NONE)
      {
        /* src2 high word is 0 */
        ot_check(th_sbc_imm(rd1, rn1, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      }
      else if (rm1 != PREG_NONE)
      {
        /* src1 high word is 0 */
        ot_check(th_mov_imm(rd1, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        ot_check(th_sbc_reg(rd1, rd1, rm1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      }
      else
      {
        /* Both high words are 0, result is derived from borrow out of low sub */
        ot_check(th_mov_imm(rd1, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        ot_check(th_sbc_imm(rd1, rd1, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      }

      /* Store and cleanup */
      if (rd_is_mem)
      {
        /* Store both low and high words for 64-bit dest */
        SValue dest_with_regs = op->dest;
        dest_with_regs.pr0 = rd0;
        dest_with_regs.pr1 = rd1;
        store(rd0, &dest_with_regs);
        /* Store high word at offset +4 */
        SValue dest_high = dest_with_regs;
        dest_high.c.i += 4;
        store(rd1, &dest_high);
        restore_scratch_reg(&rd1_alloc);
        restore_scratch_reg(&rd_alloc);
      }
      if (rn_is_mem)
      {
        restore_scratch_reg(&rn1_alloc);
        restore_scratch_reg(&rn_alloc);
      }
      if (rm_is_mem)
      {
        restore_scratch_reg(&rm1_alloc);
        restore_scratch_reg(&rm_alloc);
      }

      return;
    }
    handler.imm_handler = th_sub_imm;
    handler.reg_handler = th_sub_reg;
    break;
  case TCCIR_OP_MUL:
  {
    /* MUL instruction doesn't support immediate operands - must be register-register.
     * If src2 is an immediate, load it into a scratch register first. */
    int rm = op->src2.pr0;
    ScratchRegAlloc scratch = {0};
    if (th_has_immediate_value(op->src2.r))
    {
      /* src2 is an immediate - need to load into scratch register */
      uint32_t exclude = (1 << op->dest.pr0) | (1 << op->src1.pr0);
      scratch = get_scratch_reg_with_save(exclude);
      rm = scratch.reg;
      {
        SValue imm_sv = {0};
        imm_sv.r = VT_CONST;
        imm_sv.type.t = VT_INT;
        imm_sv.c.i = op->src2.c.i;
        load_vt_const(rm, PREG_NONE, &imm_sv);
      }
    }
    ot_check(th_mul(op->dest.pr0, op->src1.pr0, rm, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&scratch);
    return;
  }
  case TCCIR_OP_UMULL:
  {
    /* UMULL: Unsigned 32x32 multiplication producing 64-bit result
     * RdLo (dest.pr0), RdHi (dest.pr1) = Rn (src1.pr0) * Rm (src2.pr0)
     *
     * src1/src2 may be immediates or spilled; never pass PREG_NONE (0xFF)
     * into the encoder (it would become PC and fault at runtime).
     */
    int rn = op->src1.pr0;
    int rm = op->src2.pr0;
    ScratchRegAlloc rn_alloc = {0};
    ScratchRegAlloc rm_alloc = {0};
    ScratchRegAlloc rdlo_alloc = {0};
    ScratchRegAlloc rdhi_alloc = {0};

    const bool dest_is_mem = (op->dest.pr0 == PREG_NONE) || (op->dest.pr1 == PREG_NONE) ||
                             ((op->dest.pr0 & PREG_SPILLED) != 0) || ((op->dest.pr1 & PREG_SPILLED) != 0);
    int rdlo = op->dest.pr0;
    int rdhi = op->dest.pr1;

    if (rn == PREG_NONE || (rn & PREG_SPILLED) || (op->src1.r & VT_LVAL) || th_has_immediate_value(op->src1.r))
    {
      uint32_t exclude = 0;
      if (!dest_is_mem)
      {
        if (rdlo != PREG_NONE && rdlo <= 15)
          exclude |= (1u << rdlo);
        if (rdhi != PREG_NONE && rdhi <= 15)
          exclude |= (1u << rdhi);
      }
      if (rm != PREG_NONE && rm <= 15)
        exclude |= (1u << rm);
      rn_alloc = get_scratch_reg_with_save(exclude);
      load_to_reg(rn_alloc.reg, PREG_NONE, &op->src1);
      rn = rn_alloc.reg;
    }

    if (rm == PREG_NONE || (rm & PREG_SPILLED) || (op->src2.r & VT_LVAL) || th_has_immediate_value(op->src2.r))
    {
      uint32_t exclude = 0;
      if (!dest_is_mem)
      {
        if (rdlo != PREG_NONE && rdlo <= 15)
          exclude |= (1u << rdlo);
        if (rdhi != PREG_NONE && rdhi <= 15)
          exclude |= (1u << rdhi);
      }
      if (rn != PREG_NONE && rn <= 15)
        exclude |= (1u << rn);
      rm_alloc = get_scratch_reg_with_save(exclude);
      load_to_reg(rm_alloc.reg, PREG_NONE, &op->src2);
      rm = rm_alloc.reg;
    }

    if (dest_is_mem)
    {
      uint32_t exclude = 0;
      if (rn != PREG_NONE && rn <= 15)
        exclude |= (1u << rn);
      if (rm != PREG_NONE && rm <= 15)
        exclude |= (1u << rm);
      rdlo_alloc = get_scratch_reg_with_save(exclude);
      rdlo = rdlo_alloc.reg;
      exclude |= (1u << rdlo);
      rdhi_alloc = get_scratch_reg_with_save(exclude);
      rdhi = rdhi_alloc.reg;
    }

    ot_check(th_umull(rdlo, rdhi, rn, rm));

    if (dest_is_mem)
    {
      SValue dest_mem = op->dest;
      store(rdlo, &dest_mem);
      SValue dest_hi = dest_mem;
      dest_hi.c.i += 4;
      store(rdhi, &dest_hi);
    }

    restore_scratch_reg(&rdhi_alloc);
    restore_scratch_reg(&rdlo_alloc);
    restore_scratch_reg(&rm_alloc);
    restore_scratch_reg(&rn_alloc);
    return;
  }
  case TCCIR_OP_CMP:
    handler.imm_handler = th_cmp_imm;
    handler.reg_handler = th_cmp_reg;
    break;
  case TCCIR_OP_SHL:
  {
    if (is_64bit && th_has_immediate_value(op->src2.r))
    {
      const uint32_t sh = (uint32_t)op->src2.c.i;

      /* Materialize src low/high and dest low/high regs (may be spilled/mem). */
      int src_lo = op->src1.pr0;
      int src_hi = op->src1.pr1;
      ScratchRegAlloc src_lo_alloc = {0};
      ScratchRegAlloc src_hi_alloc = {0};

      const bool dest_is_mem = (op->dest.pr0 == PREG_NONE) || (op->dest.pr1 == PREG_NONE) ||
                               ((op->dest.pr0 & PREG_SPILLED) != 0) || ((op->dest.pr1 & PREG_SPILLED) != 0);
      int dst_lo = op->dest.pr0;
      int dst_hi = op->dest.pr1;
      ScratchRegAlloc dst_lo_alloc = {0};
      ScratchRegAlloc dst_hi_alloc = {0};

      uint32_t exclude = 0;
      if (!dest_is_mem)
      {
        if (dst_lo <= 15)
          exclude |= (1u << dst_lo);
        if (dst_hi <= 15)
          exclude |= (1u << dst_hi);
      }

      if (src_lo == PREG_NONE || (src_lo & PREG_SPILLED) || (op->src1.r & VT_LVAL) ||
          th_has_immediate_value(op->src1.r))
      {
        src_lo_alloc = get_scratch_reg_with_save(exclude);
        load_to_reg(src_lo_alloc.reg, PREG_NONE, &op->src1);
        src_lo = src_lo_alloc.reg;
        exclude |= (1u << src_lo);
      }

      if (src_hi == PREG_NONE)
      {
        /* Treat missing high word as 0 for left shift. */
        src_hi_alloc = get_scratch_reg_with_save(exclude);
        ot_check(th_mov_imm(src_hi_alloc.reg, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        src_hi = src_hi_alloc.reg;
        exclude |= (1u << src_hi);
      }
      else if (src_hi & PREG_SPILLED)
      {
        src_hi_alloc = get_scratch_reg_with_save(exclude);
        {
          SValue src_hi_sv = op->src1;
          src_hi_sv.pr0 = src_hi;
          src_hi_sv.pr1 = PREG_NONE;
          src_hi_sv.c.i += 4;
          load_to_reg(src_hi_alloc.reg, PREG_NONE, &src_hi_sv);
        }
        src_hi = src_hi_alloc.reg;
        exclude |= (1u << src_hi);
      }

      if (dest_is_mem)
      {
        dst_lo_alloc = get_scratch_reg_with_save(exclude);
        dst_lo = dst_lo_alloc.reg;
        exclude |= (1u << dst_lo);
        dst_hi_alloc = get_scratch_reg_with_save(exclude);
        dst_hi = dst_hi_alloc.reg;
      }

      if (sh == 0)
      {
        ot_check(th_mov_reg(dst_lo, src_lo, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                            false));
        ot_check(th_mov_reg(dst_hi, src_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                            false));
      }
      else if (sh < 32)
      {
        ScratchRegAlloc tmp_alloc = {0};
        tmp_alloc = get_scratch_reg_with_save((1u << dst_lo) | (1u << dst_hi) | (1u << src_lo) | (1u << src_hi));

        ot_check(th_lsl_imm(dst_lo, src_lo, sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        /* tmp = src_lo >> (32 - sh) */
        ot_check(th_lsr_imm(tmp_alloc.reg, src_lo, 32 - sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        /* dst_hi = (src_hi << sh) | tmp */
        ot_check(th_lsl_imm(dst_hi, src_hi, sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        ot_check(th_orr_reg(dst_hi, dst_hi, tmp_alloc.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE));

        restore_scratch_reg(&tmp_alloc);
      }
      else if (sh == 32)
      {
        ot_check(th_mov_imm(dst_lo, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        ot_check(th_mov_reg(dst_hi, src_lo, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                            false));
      }
      else if (sh < 64)
      {
        ot_check(th_mov_imm(dst_lo, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        ot_check(th_lsl_imm(dst_hi, src_lo, sh - 32, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      }
      else
      {
        ot_check(th_mov_imm(dst_lo, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        ot_check(th_mov_imm(dst_hi, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      }

      if (dest_is_mem)
      {
        SValue dest_mem = op->dest;
        store(dst_lo, &dest_mem);
        SValue dest_hi_mem = dest_mem;
        dest_hi_mem.c.i += 4;
        store(dst_hi, &dest_hi_mem);
      }

      restore_scratch_reg(&dst_hi_alloc);
      restore_scratch_reg(&dst_lo_alloc);
      restore_scratch_reg(&src_hi_alloc);
      restore_scratch_reg(&src_lo_alloc);
      return;
    }

    /* Fallback: 32-bit shift handling */
    handler.imm_handler = th_lsl_imm;
    handler.reg_handler = th_lsl_reg;
    break;
  }
  case TCCIR_OP_SHR:
  {
    if (is_64bit && th_has_immediate_value(op->src2.r))
    {
      const uint32_t sh = (uint32_t)op->src2.c.i;

      int src_lo = op->src1.pr0;
      int src_hi = op->src1.pr1;
      ScratchRegAlloc src_lo_alloc = {0};
      ScratchRegAlloc src_hi_alloc = {0};

      const bool dest_is_mem = (op->dest.pr0 == PREG_NONE) || (op->dest.pr1 == PREG_NONE) ||
                               ((op->dest.pr0 & PREG_SPILLED) != 0) || ((op->dest.pr1 & PREG_SPILLED) != 0);
      int dst_lo = op->dest.pr0;
      int dst_hi = op->dest.pr1;
      ScratchRegAlloc dst_lo_alloc = {0};
      ScratchRegAlloc dst_hi_alloc = {0};

      uint32_t exclude = 0;
      if (!dest_is_mem)
      {
        if (dst_lo <= 15)
          exclude |= (1u << dst_lo);
        if (dst_hi <= 15)
          exclude |= (1u << dst_hi);
      }

      if (src_lo == PREG_NONE || (src_lo & PREG_SPILLED) || (op->src1.r & VT_LVAL) ||
          th_has_immediate_value(op->src1.r))
      {
        src_lo_alloc = get_scratch_reg_with_save(exclude);
        load_to_reg(src_lo_alloc.reg, PREG_NONE, &op->src1);
        src_lo = src_lo_alloc.reg;
        exclude |= (1u << src_lo);
      }

      if (src_hi == PREG_NONE)
      {
        src_hi_alloc = get_scratch_reg_with_save(exclude);
        ot_check(th_mov_imm(src_hi_alloc.reg, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        src_hi = src_hi_alloc.reg;
        exclude |= (1u << src_hi);
      }
      else if (src_hi & PREG_SPILLED)
      {
        src_hi_alloc = get_scratch_reg_with_save(exclude);
        {
          SValue src_hi_sv = op->src1;
          src_hi_sv.pr0 = src_hi;
          src_hi_sv.pr1 = PREG_NONE;
          src_hi_sv.c.i += 4;
          load_to_reg(src_hi_alloc.reg, PREG_NONE, &src_hi_sv);
        }
        src_hi = src_hi_alloc.reg;
        exclude |= (1u << src_hi);
      }

      if (dest_is_mem)
      {
        dst_lo_alloc = get_scratch_reg_with_save(exclude);
        dst_lo = dst_lo_alloc.reg;
        exclude |= (1u << dst_lo);
        dst_hi_alloc = get_scratch_reg_with_save(exclude);
        dst_hi = dst_hi_alloc.reg;
      }

      if (sh == 0)
      {
        ot_check(th_mov_reg(dst_lo, src_lo, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                            false));
        ot_check(th_mov_reg(dst_hi, src_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                            false));
      }
      else if (sh < 32)
      {
        ScratchRegAlloc tmp_alloc = {0};
        tmp_alloc = get_scratch_reg_with_save((1u << dst_lo) | (1u << dst_hi) | (1u << src_lo) | (1u << src_hi));

        /* tmp = src_hi << (32 - sh) */
        ot_check(th_lsl_imm(tmp_alloc.reg, src_hi, 32 - sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        /* dst_lo = (src_lo >> sh) | tmp */
        ot_check(th_lsr_imm(dst_lo, src_lo, sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        ot_check(th_orr_reg(dst_lo, dst_lo, tmp_alloc.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE));
        /* dst_hi = src_hi >> sh */
        ot_check(th_lsr_imm(dst_hi, src_hi, sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

        restore_scratch_reg(&tmp_alloc);
      }
      else if (sh == 32)
      {
        ot_check(th_mov_reg(dst_lo, src_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                            false));
        ot_check(th_mov_imm(dst_hi, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      }
      else if (sh < 64)
      {
        ot_check(th_lsr_imm(dst_lo, src_hi, sh - 32, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        ot_check(th_mov_imm(dst_hi, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      }
      else
      {
        ot_check(th_mov_imm(dst_lo, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        ot_check(th_mov_imm(dst_hi, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      }

      if (dest_is_mem)
      {
        SValue dest_mem = op->dest;
        store(dst_lo, &dest_mem);
        SValue dest_hi_mem = dest_mem;
        dest_hi_mem.c.i += 4;
        store(dst_hi, &dest_hi_mem);
      }

      restore_scratch_reg(&dst_hi_alloc);
      restore_scratch_reg(&dst_lo_alloc);
      restore_scratch_reg(&src_hi_alloc);
      restore_scratch_reg(&src_lo_alloc);
      return;
    }

    handler.imm_handler = th_lsr_imm;
    handler.reg_handler = th_lsr_reg;
    break;
  }
  case TCCIR_OP_OR:
  {
    if (is_64bit)
    {
      const int src1_is_imm = th_has_immediate_value(op->src1.r) || op->src1.pr0 == PREG_NONE;
      const int src2_is_imm = th_has_immediate_value(op->src2.r) || op->src2.pr0 == PREG_NONE;
      const uint64_t src1_imm = (uint64_t)op->src1.c.i;
      const uint64_t src2_imm = (uint64_t)op->src2.c.i;

      /* Both constants: fold and load. */
      if (src1_is_imm && src2_is_imm)
      {
        SValue folded;
        memset(&folded, 0, sizeof(folded));
        folded.r = VT_CONST;
        folded.type = op->dest.type;
        folded.c.i = (src1_imm | src2_imm);
        load_to_dest(&op->dest, &folded);
        return;
      }

      /* One constant: prefer immediate encoding, otherwise materialize in scratch. */
      if (src1_is_imm || src2_is_imm)
      {
        const uint64_t imm64 = src1_is_imm ? src1_imm : src2_imm;
        const uint32_t imm_low = (uint32_t)(imm64 & 0xffffffffu);
        const uint32_t imm_high = (uint32_t)(imm64 >> 32);
        int reg_low = src1_is_imm ? op->src2.pr0 : op->src1.pr0;
        const int reg_high = src1_is_imm ? op->src2.pr1 : op->src1.pr1;

        int rd_low = op->dest.pr0;
        const int rd_low_needs_materialize = (rd_low == PREG_NONE) || (rd_low & PREG_SPILLED);
        ScratchRegAlloc rd_low_alloc = {0};
        ScratchRegAlloc reg_low_alloc = {0};

        if (rd_low_needs_materialize)
        {
          uint32_t exclude = 0;
          if (reg_low >= 0 && reg_low <= 15)
            exclude |= (1u << reg_low);
          if (op->dest.pr1 != PREG_NONE && op->dest.pr1 >= 0 && op->dest.pr1 <= 15)
            exclude |= (1u << op->dest.pr1);
          if (reg_high != PREG_NONE && reg_high >= 0 && reg_high <= 15)
            exclude |= (1u << reg_high);
          rd_low_alloc = get_scratch_reg_with_save(exclude);
          rd_low = rd_low_alloc.reg;
          load_to_reg(rd_low, PREG_NONE, &op->dest);
        }

        if (reg_low == PREG_NONE || (reg_low & PREG_SPILLED))
        {
          uint32_t exclude = 0;
          if (rd_low >= 0 && rd_low <= 15)
            exclude |= (1u << rd_low);
          if (op->dest.pr1 != PREG_NONE && op->dest.pr1 >= 0 && op->dest.pr1 <= 15)
            exclude |= (1u << op->dest.pr1);
          if (reg_high != PREG_NONE && reg_high >= 0 && reg_high <= 15)
            exclude |= (1u << reg_high);
          reg_low_alloc = get_scratch_reg_with_save(exclude);
          reg_low = reg_low_alloc.reg;
          load_to_reg(reg_low, PREG_NONE, src1_is_imm ? &op->src2 : &op->src1);
        }

        /* Low word */
        thumb_opcode or_low =
            th_orr_imm(rd_low, reg_low, imm_low, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
        if (or_low.size == 0)
        {
          ScratchRegAlloc scratch = {0};
          uint32_t exclude = 0;
          if (rd_low >= 0 && rd_low <= 15)
            exclude |= (1u << rd_low);
          if (reg_low >= 0 && reg_low <= 15)
            exclude |= (1u << reg_low);
          if (op->dest.pr1 != PREG_NONE && op->dest.pr1 >= 0 && op->dest.pr1 <= 15)
            exclude |= (1u << op->dest.pr1);
          if (reg_high != PREG_NONE && reg_high >= 0 && reg_high <= 15)
            exclude |= (1u << reg_high);
          scratch = get_scratch_reg_with_save(exclude);
          SValue imm_sv;
          memset(&imm_sv, 0, sizeof(imm_sv));
          imm_sv.r = VT_CONST;
          imm_sv.type.t = VT_INT | VT_UNSIGNED;
          imm_sv.c.i = imm_low;
          load_to_reg(scratch.reg, PREG_NONE, &imm_sv);
          ot_check(th_orr_reg(rd_low, reg_low, scratch.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE));
          restore_scratch_reg(&scratch);
        }
        else
        {
          ot_check(or_low);
        }

        if (rd_low_needs_materialize)
          store(rd_low, &op->dest);
        restore_scratch_reg(&reg_low_alloc);
        restore_scratch_reg(&rd_low_alloc);

        /* High word: treat missing high half as 0. */
        if (op->dest.pr1 != PREG_NONE)
        {
          if (reg_high == PREG_NONE)
          {
            if (imm_high == 0)
            {
              ot_check(th_mov_imm(op->dest.pr1, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
            }
            else
            {
              SValue imm_hi_sv;
              memset(&imm_hi_sv, 0, sizeof(imm_hi_sv));
              imm_hi_sv.r = VT_CONST;
              imm_hi_sv.type.t = VT_INT | VT_UNSIGNED;
              imm_hi_sv.c.i = imm_high;
              load_to_reg(op->dest.pr1, PREG_NONE, &imm_hi_sv);
            }
          }
          else
          {
            if (imm_high == 0)
            {
              if (op->dest.pr1 != reg_high)
                ot_check(th_mov_reg(op->dest.pr1, reg_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                    ENFORCE_ENCODING_NONE, false));
            }
            else
            {
              thumb_opcode or_high =
                  th_orr_imm(op->dest.pr1, reg_high, imm_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
              if (or_high.size == 0)
              {
                ScratchRegAlloc scratch = {0};
                uint32_t exclude = (1u << op->dest.pr1) | (1u << reg_high);
                exclude |= (1u << op->dest.pr0) | (1u << reg_low);
                scratch = get_scratch_reg_with_save(exclude);
                SValue imm_sv;
                memset(&imm_sv, 0, sizeof(imm_sv));
                imm_sv.r = VT_CONST;
                imm_sv.type.t = VT_INT | VT_UNSIGNED;
                imm_sv.c.i = imm_high;
                load_to_reg(scratch.reg, PREG_NONE, &imm_sv);
                ot_check(th_orr_reg(op->dest.pr1, reg_high, scratch.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                                    THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
                restore_scratch_reg(&scratch);
              }
              else
              {
                ot_check(or_high);
              }
            }
          }
        }

        return;
      }

      /* 64-bit OR: OR both halves */
      /* Low word always ORed */
      {
        int rd = op->dest.pr0;
        int rn = op->src1.pr0;
        int rm = op->src2.pr0;
        ScratchRegAlloc rd_alloc = {0};
        ScratchRegAlloc rn_alloc = {0};
        ScratchRegAlloc rm_alloc = {0};

        if (rd == PREG_NONE || (rd & PREG_SPILLED))
        {
          uint32_t exclude = 0;
          if (rn >= 0 && rn <= 15)
            exclude |= (1u << rn);
          if (rm >= 0 && rm <= 15)
            exclude |= (1u << rm);
          rd_alloc = get_scratch_reg_with_save(exclude);
          rd = rd_alloc.reg;
          load_to_reg(rd, PREG_NONE, &op->dest);
        }

        if (rn == PREG_NONE || (rn & PREG_SPILLED))
        {
          uint32_t exclude = (1u << rd);
          if (rm >= 0 && rm <= 15)
            exclude |= (1u << rm);
          rn_alloc = get_scratch_reg_with_save(exclude);
          rn = rn_alloc.reg;
          load_to_reg(rn, PREG_NONE, &op->src1);
        }

        if (rm == PREG_NONE || (rm & PREG_SPILLED))
        {
          uint32_t exclude = (1u << rd);
          if (rn >= 0 && rn <= 15)
            exclude |= (1u << rn);
          rm_alloc = get_scratch_reg_with_save(exclude);
          rm = rm_alloc.reg;
          load_to_reg(rm, PREG_NONE, &op->src2);
        }

        ot_check(th_orr_reg(rd, rn, rm, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));

        if (rd_alloc.saved)
          store(rd, &op->dest);
        restore_scratch_reg(&rm_alloc);
        restore_scratch_reg(&rn_alloc);
        restore_scratch_reg(&rd_alloc);
      }
      /* High word: handle mixed 32/64-bit operands */
      /* For OR: 32-bit value has 0 in high word, ORing with 0 = original */
      {
        const int src1_is64 = is_64bit_type(op->src1.type.t);
        const int src2_is64 = is_64bit_type(op->src2.type.t);

        int rd_hi = op->dest.pr1;
        int rn_hi = op->src1.pr1;
        int rm_hi = op->src2.pr1;

        ScratchRegAlloc rd_hi_alloc = {0};
        ScratchRegAlloc rn_hi_alloc = {0};
        ScratchRegAlloc rm_hi_alloc = {0};

        /* Ensure we never pass PREG_NONE/spilled to opcode encoders. */
        if (rd_hi == PREG_NONE || (rd_hi & PREG_SPILLED))
        {
          uint32_t exclude = (1u << R_SP);
          if (op->dest.pr0 >= 0 && op->dest.pr0 <= 15)
            exclude |= (1u << op->dest.pr0);
          if (op->src1.pr0 >= 0 && op->src1.pr0 <= 15)
            exclude |= (1u << op->src1.pr0);
          if (op->src2.pr0 >= 0 && op->src2.pr0 <= 15)
            exclude |= (1u << op->src2.pr0);
          if (rn_hi >= 0 && rn_hi <= 15)
            exclude |= (1u << rn_hi);
          if (rm_hi >= 0 && rm_hi <= 15)
            exclude |= (1u << rm_hi);
          rd_hi_alloc = get_scratch_reg_with_save(exclude);
          rd_hi = rd_hi_alloc.reg;
        }

        if (src1_is64 && (rn_hi == PREG_NONE || (rn_hi & PREG_SPILLED)))
        {
          uint32_t exclude = (1u << R_SP);
          if (rd_hi >= 0 && rd_hi <= 15)
            exclude |= (1u << rd_hi);
          if (op->dest.pr0 >= 0 && op->dest.pr0 <= 15)
            exclude |= (1u << op->dest.pr0);
          if (op->src1.pr0 >= 0 && op->src1.pr0 <= 15)
            exclude |= (1u << op->src1.pr0);
          if (op->src2.pr0 >= 0 && op->src2.pr0 <= 15)
            exclude |= (1u << op->src2.pr0);
          if (rm_hi >= 0 && rm_hi <= 15)
            exclude |= (1u << rm_hi);
          rn_hi_alloc = get_scratch_reg_with_save(exclude);
          rn_hi = rn_hi_alloc.reg;

          SValue src1_hi_sv = op->src1;
          src1_hi_sv.type.t = (src1_hi_sv.type.t & ~VT_BTYPE) | VT_INT | VT_UNSIGNED;
          src1_hi_sv.c.i += 4;
          load_to_reg(rn_hi, PREG_NONE, &src1_hi_sv);
        }

        if (src2_is64 && (rm_hi == PREG_NONE || (rm_hi & PREG_SPILLED)))
        {
          uint32_t exclude = (1u << R_SP);
          if (rd_hi >= 0 && rd_hi <= 15)
            exclude |= (1u << rd_hi);
          if (rn_hi >= 0 && rn_hi <= 15)
            exclude |= (1u << rn_hi);
          if (op->dest.pr0 >= 0 && op->dest.pr0 <= 15)
            exclude |= (1u << op->dest.pr0);
          if (op->src1.pr0 >= 0 && op->src1.pr0 <= 15)
            exclude |= (1u << op->src1.pr0);
          if (op->src2.pr0 >= 0 && op->src2.pr0 <= 15)
            exclude |= (1u << op->src2.pr0);
          rm_hi_alloc = get_scratch_reg_with_save(exclude);
          rm_hi = rm_hi_alloc.reg;

          SValue src2_hi_sv = op->src2;
          src2_hi_sv.type.t = (src2_hi_sv.type.t & ~VT_BTYPE) | VT_INT | VT_UNSIGNED;
          src2_hi_sv.c.i += 4;
          load_to_reg(rm_hi, PREG_NONE, &src2_hi_sv);
        }

        /* Compute high word with proper 32/64-bit mixing semantics. */
        if (!src1_is64 && !src2_is64)
        {
          ot_check(th_mov_imm(rd_hi, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        }
        else if (!src2_is64)
        {
          /* src2 high is 0 => result high = src1 high */
          if (rn_hi == PREG_NONE)
            ot_check(th_mov_imm(rd_hi, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
          else if (rd_hi != rn_hi)
            ot_check(th_mov_reg(rd_hi, rn_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                                false));
        }
        else if (!src1_is64)
        {
          /* src1 high is 0 => result high = src2 high */
          if (rm_hi == PREG_NONE)
            ot_check(th_mov_imm(rd_hi, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
          else if (rd_hi != rm_hi)
            ot_check(th_mov_reg(rd_hi, rm_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                                false));
        }
        else
        {
          /* Both operands are 64-bit */
          ot_check(th_orr_reg(rd_hi, rn_hi, rm_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE));
        }

        if (rd_hi_alloc.saved)
        {
          SValue dest_hi_sv = op->dest;
          dest_hi_sv.type.t = (dest_hi_sv.type.t & ~VT_BTYPE) | VT_INT | VT_UNSIGNED;
          dest_hi_sv.c.i += 4;
          store(rd_hi, &dest_hi_sv);
        }

        restore_scratch_reg(&rm_hi_alloc);
        restore_scratch_reg(&rn_hi_alloc);
        restore_scratch_reg(&rd_hi_alloc);
      }
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
      const int src1_is_imm = th_has_immediate_value(op->src1.r) || op->src1.pr0 == PREG_NONE;
      const int src2_is_imm = th_has_immediate_value(op->src2.r) || op->src2.pr0 == PREG_NONE;
      const uint64_t src1_imm = (uint64_t)op->src1.c.i;
      const uint64_t src2_imm = (uint64_t)op->src2.c.i;

      /* Both constants: fold and load. */
      if (src1_is_imm && src2_is_imm)
      {
        SValue folded;
        memset(&folded, 0, sizeof(folded));
        folded.r = VT_CONST;
        folded.type = op->dest.type;
        folded.c.i = (src1_imm & src2_imm);
        load_to_dest(&op->dest, &folded);
        return;
      }

      /* One constant: prefer immediate encoding, otherwise materialize in scratch. */
      if (src1_is_imm || src2_is_imm)
      {
        const uint64_t imm64 = src1_is_imm ? src1_imm : src2_imm;
        const uint32_t imm_low = (uint32_t)(imm64 & 0xffffffffu);
        const uint32_t imm_high = (uint32_t)(imm64 >> 32);
        int reg_low = src1_is_imm ? op->src2.pr0 : op->src1.pr0;
        int reg_high = src1_is_imm ? op->src2.pr1 : op->src1.pr1;

        /* Handle memory/spilled operands */
        int rd_low = op->dest.pr0;
        int rd_high = op->dest.pr1;
        int rd_low_is_mem = (rd_low == PREG_NONE) || (rd_low & PREG_SPILLED);
        int rd_high_is_mem = (rd_high == PREG_NONE) || (rd_high & PREG_SPILLED);
        int reg_low_is_mem = (reg_low == PREG_NONE) || (reg_low & PREG_SPILLED);
        int reg_high_is_mem = (reg_high == PREG_NONE) || (reg_high & PREG_SPILLED);

        ScratchRegAlloc rd_low_alloc = {0};
        ScratchRegAlloc rd_high_alloc = {0};
        ScratchRegAlloc reg_low_alloc = {0};
        ScratchRegAlloc reg_high_alloc = {0};

        uint32_t exclude = (1u << R_SP);
        if (!rd_low_is_mem && rd_low >= 0 && rd_low <= 15)
          exclude |= (1u << rd_low);
        if (!rd_high_is_mem && rd_high >= 0 && rd_high <= 15)
          exclude |= (1u << rd_high);
        if (!reg_low_is_mem && reg_low >= 0 && reg_low <= 15)
          exclude |= (1u << reg_low);
        if (!reg_high_is_mem && reg_high >= 0 && reg_high <= 15)
          exclude |= (1u << reg_high);

        if (rd_low_is_mem)
        {
          rd_low_alloc = get_scratch_reg_with_save(exclude);
          rd_low = rd_low_alloc.reg;
          exclude |= (1u << rd_low);
        }
        if (rd_high_is_mem && op->dest.pr1 != PREG_NONE)
        {
          rd_high_alloc = get_scratch_reg_with_save(exclude);
          rd_high = rd_high_alloc.reg;
          exclude |= (1u << rd_high);
        }
        if (reg_low_is_mem)
        {
          reg_low_alloc = get_scratch_reg_with_save(exclude);
          reg_low = reg_low_alloc.reg;
          exclude |= (1u << reg_low);
          load_to_reg(reg_low, PREG_NONE, src1_is_imm ? &op->src2 : &op->src1);
        }
        if (reg_high_is_mem && (src1_is_imm ? op->src2.pr1 : op->src1.pr1) != PREG_NONE)
        {
          reg_high_alloc = get_scratch_reg_with_save(exclude);
          reg_high = reg_high_alloc.reg;
          exclude |= (1u << reg_high);
          SValue src_hi = src1_is_imm ? op->src2 : op->src1;
          src_hi.c.i += 4;
          load_to_reg(reg_high, PREG_NONE, &src_hi);
        }

        /* Low word */
        thumb_opcode and_low =
            th_and_imm(rd_low, reg_low, imm_low, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
        if (and_low.size == 0)
        {
          ScratchRegAlloc scratch = {0};
          uint32_t excl2 = exclude;
          scratch = get_scratch_reg_with_save(excl2);
          SValue imm_sv;
          memset(&imm_sv, 0, sizeof(imm_sv));
          imm_sv.r = VT_CONST;
          imm_sv.type.t = VT_INT | VT_UNSIGNED;
          imm_sv.c.i = imm_low;
          load_to_reg(scratch.reg, PREG_NONE, &imm_sv);
          ot_check(th_and_reg(rd_low, reg_low, scratch.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE));
          restore_scratch_reg(&scratch);
        }
        else
        {
          ot_check(and_low);
        }

        /* Store low result if needed */
        if (rd_low_is_mem)
          store(rd_low, &op->dest);

        /* High word: treat missing high half as 0. For AND, any 32-bit operand forces high word to 0. */
        if (op->dest.pr1 != PREG_NONE)
        {
          if ((src1_is_imm ? op->src2.pr1 : op->src1.pr1) == PREG_NONE || imm_high == 0)
          {
            ot_check(th_mov_imm(rd_high, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
          }
          else
          {
            thumb_opcode and_high =
                th_and_imm(rd_high, reg_high, imm_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
            if (and_high.size == 0)
            {
              ScratchRegAlloc scratch = {0};
              uint32_t excl2 = exclude;
              scratch = get_scratch_reg_with_save(excl2);
              SValue imm_sv;
              memset(&imm_sv, 0, sizeof(imm_sv));
              imm_sv.r = VT_CONST;
              imm_sv.type.t = VT_INT | VT_UNSIGNED;
              imm_sv.c.i = imm_high;
              load_to_reg(scratch.reg, PREG_NONE, &imm_sv);
              ot_check(th_and_reg(rd_high, reg_high, scratch.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                  ENFORCE_ENCODING_NONE));
              restore_scratch_reg(&scratch);
            }
            else
            {
              ot_check(and_high);
            }
          }

          /* Store high result if needed */
          if (rd_high_is_mem)
          {
            SValue dest_hi = op->dest;
            dest_hi.c.i += 4;
            store(rd_high, &dest_hi);
          }
        }

        /* Restore scratch regs */
        restore_scratch_reg(&reg_high_alloc);
        restore_scratch_reg(&reg_low_alloc);
        restore_scratch_reg(&rd_high_alloc);
        restore_scratch_reg(&rd_low_alloc);

        return;
      }

      /* 64-bit AND: AND both halves */
      /* Handle memory destinations and spilled operands */
      {
        int rd0 = op->dest.pr0;
        int rd1 = op->dest.pr1;
        int rn0 = op->src1.pr0;
        int rn1 = op->src1.pr1;
        int rm0 = op->src2.pr0;
        int rm1 = op->src2.pr1;

        ScratchRegAlloc rd0_alloc = {0};
        ScratchRegAlloc rd1_alloc = {0};
        ScratchRegAlloc rn0_alloc = {0};
        ScratchRegAlloc rn1_alloc = {0};
        ScratchRegAlloc rm0_alloc = {0};
        ScratchRegAlloc rm1_alloc = {0};

        uint32_t exclude = (1u << R_SP);

        /* Handle dest */
        int rd0_is_mem = (rd0 == PREG_NONE) || (rd0 & PREG_SPILLED);
        int rd1_is_mem = (rd1 == PREG_NONE) || (rd1 & PREG_SPILLED);

        /* Handle src1 */
        int rn0_is_mem = (rn0 == PREG_NONE) || (rn0 & PREG_SPILLED);
        int rn1_is_mem = (rn1 == PREG_NONE) || (rn1 & PREG_SPILLED);

        /* Handle src2 */
        int rm0_is_mem = (rm0 == PREG_NONE) || (rm0 & PREG_SPILLED);
        int rm1_is_mem = (rm1 == PREG_NONE) || (rm1 & PREG_SPILLED);

        /* Build exclude mask for valid registers */
        if (!rd0_is_mem && rd0 >= 0 && rd0 <= 15)
          exclude |= (1u << rd0);
        if (!rd1_is_mem && rd1 >= 0 && rd1 <= 15)
          exclude |= (1u << rd1);
        if (!rn0_is_mem && rn0 >= 0 && rn0 <= 15)
          exclude |= (1u << rn0);
        if (!rn1_is_mem && rn1 >= 0 && rn1 <= 15)
          exclude |= (1u << rn1);
        if (!rm0_is_mem && rm0 >= 0 && rm0 <= 15)
          exclude |= (1u << rm0);
        if (!rm1_is_mem && rm1 >= 0 && rm1 <= 15)
          exclude |= (1u << rm1);

        /* Allocate scratch regs for memory operands */
        if (rd0_is_mem)
        {
          rd0_alloc = get_scratch_reg_with_save(exclude);
          rd0 = rd0_alloc.reg;
          exclude |= (1u << rd0);
        }
        if (rd1_is_mem && op->dest.pr1 != PREG_NONE)
        {
          rd1_alloc = get_scratch_reg_with_save(exclude);
          rd1 = rd1_alloc.reg;
          exclude |= (1u << rd1);
        }
        if (rn0_is_mem)
        {
          rn0_alloc = get_scratch_reg_with_save(exclude);
          rn0 = rn0_alloc.reg;
          exclude |= (1u << rn0);
          load_to_reg(rn0, PREG_NONE, &op->src1);
        }
        if (rn1_is_mem && op->src1.pr1 != PREG_NONE)
        {
          rn1_alloc = get_scratch_reg_with_save(exclude);
          rn1 = rn1_alloc.reg;
          exclude |= (1u << rn1);
          SValue src1_hi = op->src1;
          src1_hi.c.i += 4;
          load_to_reg(rn1, PREG_NONE, &src1_hi);
        }
        if (rm0_is_mem)
        {
          rm0_alloc = get_scratch_reg_with_save(exclude);
          rm0 = rm0_alloc.reg;
          exclude |= (1u << rm0);
          load_to_reg(rm0, PREG_NONE, &op->src2);
        }
        if (rm1_is_mem && op->src2.pr1 != PREG_NONE)
        {
          rm1_alloc = get_scratch_reg_with_save(exclude);
          rm1 = rm1_alloc.reg;
          exclude |= (1u << rm1);
          SValue src2_hi = op->src2;
          src2_hi.c.i += 4;
          load_to_reg(rm1, PREG_NONE, &src2_hi);
        }

        /* Low word always ANDed */
        ot_check(th_and_reg(rd0, rn0, rm0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));

        /* High word: handle mixed 32/64-bit operands */
        /* For AND: 32-bit value has 0 in high word, ANDing with 0 = 0 */
        if (op->src1.pr1 == PREG_NONE || op->src2.pr1 == PREG_NONE)
        {
          /* Either operand is 32-bit, high word becomes 0 */
          if (op->dest.pr1 != PREG_NONE)
            ot_check(th_mov_imm(rd1, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        }
        else
        {
          /* Both operands are 64-bit */
          ot_check(
              th_and_reg(rd1, rn1, rm1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        }

        /* Store results to memory if needed */
        if (rd0_is_mem)
        {
          store(rd0, &op->dest);
        }
        if (rd1_is_mem && op->dest.pr1 != PREG_NONE)
        {
          SValue dest_hi = op->dest;
          dest_hi.c.i += 4;
          store(rd1, &dest_hi);
        }

        /* Restore scratch regs */
        restore_scratch_reg(&rm1_alloc);
        restore_scratch_reg(&rm0_alloc);
        restore_scratch_reg(&rn1_alloc);
        restore_scratch_reg(&rn0_alloc);
        restore_scratch_reg(&rd1_alloc);
        restore_scratch_reg(&rd0_alloc);
      }
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
      const int src1_is_imm = th_has_immediate_value(op->src1.r) || op->src1.pr0 == PREG_NONE;
      const int src2_is_imm = th_has_immediate_value(op->src2.r) || op->src2.pr0 == PREG_NONE;
      const uint64_t src1_imm = (uint64_t)op->src1.c.i;
      const uint64_t src2_imm = (uint64_t)op->src2.c.i;

      /* Both constants: fold and load. */
      if (src1_is_imm && src2_is_imm)
      {
        SValue folded;
        memset(&folded, 0, sizeof(folded));
        folded.r = VT_CONST;
        folded.type = op->dest.type;
        folded.c.i = (src1_imm ^ src2_imm);
        load_to_dest(&op->dest, &folded);
        return;
      }

      /* One constant: prefer immediate encoding, otherwise materialize in scratch. */
      if (src1_is_imm || src2_is_imm)
      {
        const uint64_t imm64 = src1_is_imm ? src1_imm : src2_imm;
        const uint32_t imm_low = (uint32_t)(imm64 & 0xffffffffu);
        const uint32_t imm_high = (uint32_t)(imm64 >> 32);
        const int reg_low = src1_is_imm ? op->src2.pr0 : op->src1.pr0;
        const int reg_high = src1_is_imm ? op->src2.pr1 : op->src1.pr1;

        /* Low word */
        thumb_opcode xor_low =
            th_eor_imm(op->dest.pr0, reg_low, imm_low, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
        if (xor_low.size == 0)
        {
          ScratchRegAlloc scratch = {0};
          uint32_t exclude = (1u << op->dest.pr0) | (1u << reg_low);
          if (op->dest.pr1 != PREG_NONE)
            exclude |= (1u << op->dest.pr1);
          if (reg_high != PREG_NONE)
            exclude |= (1u << reg_high);
          scratch = get_scratch_reg_with_save(exclude);
          SValue imm_sv;
          memset(&imm_sv, 0, sizeof(imm_sv));
          imm_sv.r = VT_CONST;
          imm_sv.type.t = VT_INT | VT_UNSIGNED;
          imm_sv.c.i = imm_low;
          load_to_reg(scratch.reg, PREG_NONE, &imm_sv);
          ot_check(th_eor_reg(op->dest.pr0, reg_low, scratch.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE));
          restore_scratch_reg(&scratch);
        }
        else
        {
          ot_check(xor_low);
        }

        /* High word: treat missing high half as 0. */
        if (op->dest.pr1 != PREG_NONE)
        {
          if (reg_high == PREG_NONE)
          {
            if (imm_high == 0)
            {
              ot_check(th_mov_imm(op->dest.pr1, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
            }
            else
            {
              SValue imm_hi_sv;
              memset(&imm_hi_sv, 0, sizeof(imm_hi_sv));
              imm_hi_sv.r = VT_CONST;
              imm_hi_sv.type.t = VT_INT | VT_UNSIGNED;
              imm_hi_sv.c.i = imm_high;
              load_to_reg(op->dest.pr1, PREG_NONE, &imm_hi_sv);
            }
          }
          else
          {
            if (imm_high == 0)
            {
              if (op->dest.pr1 != reg_high)
                ot_check(th_mov_reg(op->dest.pr1, reg_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                    ENFORCE_ENCODING_NONE, false));
            }
            else
            {
              thumb_opcode xor_high =
                  th_eor_imm(op->dest.pr1, reg_high, imm_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
              if (xor_high.size == 0)
              {
                ScratchRegAlloc scratch = {0};
                uint32_t exclude = (1u << op->dest.pr1) | (1u << reg_high);
                exclude |= (1u << op->dest.pr0) | (1u << reg_low);
                scratch = get_scratch_reg_with_save(exclude);
                SValue imm_sv;
                memset(&imm_sv, 0, sizeof(imm_sv));
                imm_sv.r = VT_CONST;
                imm_sv.type.t = VT_INT | VT_UNSIGNED;
                imm_sv.c.i = imm_high;
                load_to_reg(scratch.reg, PREG_NONE, &imm_sv);
                ot_check(th_eor_reg(op->dest.pr1, reg_high, scratch.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                                    THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
                restore_scratch_reg(&scratch);
              }
              else
              {
                ot_check(xor_high);
              }
            }
          }
        }

        return;
      }

      /* 64-bit XOR: XOR both halves */
      /* Low word always XORed */
      ot_check(th_eor_reg(op->dest.pr0, op->src1.pr0, op->src2.pr0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      /* High word: handle mixed 32/64-bit operands */
      /* For XOR: 32-bit value has 0 in high word, XORing with 0 = original */
      if (op->src1.pr1 == PREG_NONE && op->src2.pr1 == PREG_NONE)
      {
        /* Both operands are 32-bit, high word is 0 */
        ot_check(th_mov_imm(op->dest.pr1, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      }
      else if (op->src2.pr1 == PREG_NONE)
      {
        /* src2 is 32-bit, just copy src1's high word */
        if (op->dest.pr1 != op->src1.pr1)
          ot_check(th_mov_reg(op->dest.pr1, op->src1.pr1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE, false));
      }
      else if (op->src1.pr1 == PREG_NONE)
      {
        /* src1 is 32-bit, just copy src2's high word */
        if (op->dest.pr1 != op->src2.pr1)
          ot_check(th_mov_reg(op->dest.pr1, op->src2.pr1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE, false));
      }
      else
      {
        /* Both operands are 64-bit */
        ot_check(th_eor_reg(op->dest.pr1, op->src1.pr1, op->src2.pr1, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                            THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      }
      return;
    }
    handler.imm_handler = th_eor_imm;
    handler.reg_handler = th_eor_reg;
    break;
  }
  case TCCIR_OP_SAR:
  {
    if (is_64bit && th_has_immediate_value(op->src2.r))
    {
      const uint32_t sh = (uint32_t)op->src2.c.i;

      int src_lo = op->src1.pr0;
      int src_hi = op->src1.pr1;
      ScratchRegAlloc src_lo_alloc = {0};
      ScratchRegAlloc src_hi_alloc = {0};

      const bool dest_is_mem = (op->dest.pr0 == PREG_NONE) || (op->dest.pr1 == PREG_NONE) ||
                               ((op->dest.pr0 & PREG_SPILLED) != 0) || ((op->dest.pr1 & PREG_SPILLED) != 0);
      int dst_lo = op->dest.pr0;
      int dst_hi = op->dest.pr1;
      ScratchRegAlloc dst_lo_alloc = {0};
      ScratchRegAlloc dst_hi_alloc = {0};

      uint32_t exclude = 0;
      if (!dest_is_mem)
      {
        if (dst_lo <= 15)
          exclude |= (1u << dst_lo);
        if (dst_hi <= 15)
          exclude |= (1u << dst_hi);
      }

      if (src_lo == PREG_NONE || (src_lo & PREG_SPILLED) || (op->src1.r & VT_LVAL) ||
          th_has_immediate_value(op->src1.r))
      {
        src_lo_alloc = get_scratch_reg_with_save(exclude);
        load_to_reg(src_lo_alloc.reg, PREG_NONE, &op->src1);
        src_lo = src_lo_alloc.reg;
        exclude |= (1u << src_lo);
      }

      if (src_hi == PREG_NONE)
      {
        /* Sign-extend missing high word from src_lo. */
        src_hi_alloc = get_scratch_reg_with_save(exclude);
        ot_check(th_asr_imm(src_hi_alloc.reg, src_lo, 31, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        src_hi = src_hi_alloc.reg;
        exclude |= (1u << src_hi);
      }
      else if (src_hi & PREG_SPILLED)
      {
        src_hi_alloc = get_scratch_reg_with_save(exclude);
        {
          SValue src_hi_sv = op->src1;
          src_hi_sv.pr0 = src_hi;
          src_hi_sv.pr1 = PREG_NONE;
          src_hi_sv.c.i += 4;
          load_to_reg(src_hi_alloc.reg, PREG_NONE, &src_hi_sv);
        }
        src_hi = src_hi_alloc.reg;
        exclude |= (1u << src_hi);
      }

      if (dest_is_mem)
      {
        dst_lo_alloc = get_scratch_reg_with_save(exclude);
        dst_lo = dst_lo_alloc.reg;
        exclude |= (1u << dst_lo);
        dst_hi_alloc = get_scratch_reg_with_save(exclude);
        dst_hi = dst_hi_alloc.reg;
      }

      if (sh == 0)
      {
        ot_check(th_mov_reg(dst_lo, src_lo, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                            false));
        ot_check(th_mov_reg(dst_hi, src_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                            false));
      }
      else if (sh < 32)
      {
        ScratchRegAlloc tmp_alloc = {0};
        tmp_alloc = get_scratch_reg_with_save((1u << dst_lo) | (1u << dst_hi) | (1u << src_lo) | (1u << src_hi));

        /* tmp = src_hi << (32 - sh) */
        ot_check(th_lsl_imm(tmp_alloc.reg, src_hi, 32 - sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        /* dst_lo = (src_lo >> sh) | tmp */
        ot_check(th_lsr_imm(dst_lo, src_lo, sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        ot_check(th_orr_reg(dst_lo, dst_lo, tmp_alloc.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE));
        /* dst_hi = src_hi >> sh (arith) */
        ot_check(th_asr_imm(dst_hi, src_hi, sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

        restore_scratch_reg(&tmp_alloc);
      }
      else if (sh == 32)
      {
        ot_check(th_mov_reg(dst_lo, src_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                            false));
        ot_check(th_asr_imm(dst_hi, src_hi, 31, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      }
      else if (sh < 64)
      {
        ot_check(th_asr_imm(dst_lo, src_hi, sh - 32, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        ot_check(th_asr_imm(dst_hi, src_hi, 31, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      }
      else
      {
        ot_check(th_asr_imm(dst_hi, src_hi, 31, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
        ot_check(th_mov_reg(dst_lo, dst_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                            false));
      }

      if (dest_is_mem)
      {
        SValue dest_mem = op->dest;
        store(dst_lo, &dest_mem);
        SValue dest_hi_mem = dest_mem;
        dest_hi_mem.c.i += 4;
        store(dst_hi, &dest_hi_mem);
      }

      restore_scratch_reg(&dst_hi_alloc);
      restore_scratch_reg(&dst_lo_alloc);
      restore_scratch_reg(&src_hi_alloc);
      restore_scratch_reg(&src_lo_alloc);
      return;
    }

    handler.imm_handler = th_asr_imm;
    handler.reg_handler = th_asr_reg;
    break;
  }
  case TCCIR_OP_DIV:
  {
    int src1_reg = op->src1.pr0;
    int src2_reg = op->src2.pr0;
    uint32_t exclude_regs = (1 << op->dest.pr0);
    ScratchRegAlloc src1_alloc = {0};
    ScratchRegAlloc src2_alloc = {0};

    /* Handle constant operands - DIV has no immediate form */
    if (th_has_immediate_value(op->src1.r))
    {
      src1_alloc = get_scratch_reg_with_save(exclude_regs);
      src1_reg = src1_alloc.reg;
      exclude_regs |= (1 << src1_reg);
      load_to_reg(src1_reg, PREG_NONE, &op->src1);
    }
    else if (src1_reg >= 0)
    {
      exclude_regs |= (1 << src1_reg);
    }
    if (th_has_immediate_value(op->src2.r))
    {
      src2_alloc = get_scratch_reg_with_save(exclude_regs);
      src2_reg = src2_alloc.reg;
      load_to_reg(src2_reg, PREG_NONE, &op->src2);
    }
    ot_check(th_sdiv(op->dest.pr0, src1_reg, src2_reg));

    /* Restore allocated scratches */
    if (src2_alloc.reg != 0)
      restore_scratch_reg(&src2_alloc);
    if (src1_alloc.reg != 0)
      restore_scratch_reg(&src1_alloc);
    return;
  }
  case TCCIR_OP_UDIV:
  {
    int src1_reg = op->src1.pr0;
    int src2_reg = op->src2.pr0;
    uint32_t exclude_regs = (1 << op->dest.pr0);
    ScratchRegAlloc src1_alloc = {0};
    ScratchRegAlloc src2_alloc = {0};

    /* Handle constant operands - UDIV has no immediate form */
    if (th_has_immediate_value(op->src1.r))
    {
      src1_alloc = get_scratch_reg_with_save(exclude_regs);
      src1_reg = src1_alloc.reg;
      exclude_regs |= (1 << src1_reg);
      load_to_reg(src1_reg, PREG_NONE, &op->src1);
    }
    else if (src1_reg >= 0)
    {
      exclude_regs |= (1 << src1_reg);
    }
    if (th_has_immediate_value(op->src2.r))
    {
      src2_alloc = get_scratch_reg_with_save(exclude_regs);
      src2_reg = src2_alloc.reg;
      load_to_reg(src2_reg, PREG_NONE, &op->src2);
    }
    ot_check(th_udiv(op->dest.pr0, src1_reg, src2_reg));

    /* Restore allocated scratches */
    if (src2_alloc.reg != 0)
      restore_scratch_reg(&src2_alloc);
    if (src1_alloc.reg != 0)
      restore_scratch_reg(&src1_alloc);
    return;
  }
  case TCCIR_OP_IMOD:
  {
    /* Signed modulo: result = dividend - (dividend / divisor) * divisor */
    int src1_reg = op->src1.pr0;
    int src2_reg = op->src2.pr0;
    int dest_reg = op->dest.pr0;
    uint32_t exclude_regs = (1 << dest_reg);
    ScratchRegAlloc src1_alloc = {0};
    ScratchRegAlloc src2_alloc = {0};
    ScratchRegAlloc scratch_alloc = {0};

    /* Handle constant operands */
    if (th_has_immediate_value(op->src1.r))
    {
      src1_alloc = get_scratch_reg_with_save(exclude_regs);
      src1_reg = src1_alloc.reg;
      exclude_regs |= (1 << src1_reg);
      load_to_reg(src1_reg, PREG_NONE, &op->src1);
    }
    else if (src1_reg >= 0)
    {
      exclude_regs |= (1 << src1_reg);
    }
    if (th_has_immediate_value(op->src2.r))
    {
      src2_alloc = get_scratch_reg_with_save(exclude_regs);
      src2_reg = src2_alloc.reg;
      exclude_regs |= (1 << src2_reg);
      load_to_reg(src2_reg, PREG_NONE, &op->src2);
    }
    else if (src2_reg >= 0)
    {
      exclude_regs |= (1 << src2_reg);
    }

    /* Get scratch register for quotient */
    scratch_alloc = get_scratch_reg_with_save(exclude_regs);
    int scratch = scratch_alloc.reg;

    /* quotient = dividend / divisor (signed) */
    ot_check(th_sdiv(scratch, src1_reg, src2_reg));
    /* quotient = quotient * divisor */
    ot_check(th_mul(scratch, scratch, src2_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    /* result = dividend - quotient */
    ot_check(th_sub_reg(dest_reg, src1_reg, scratch, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                        ENFORCE_ENCODING_NONE));

    /* Restore allocated scratches in reverse order */
    restore_scratch_reg(&scratch_alloc);
    if (src2_alloc.reg != 0)
      restore_scratch_reg(&src2_alloc);
    if (src1_alloc.reg != 0)
      restore_scratch_reg(&src1_alloc);
    return;
  }
  case TCCIR_OP_UMOD:
  {
    /* Unsigned modulo: result = dividend - (dividend / divisor) * divisor */
    int src1_reg = op->src1.pr0;
    int src2_reg = op->src2.pr0;
    int dest_reg = op->dest.pr0;
    uint32_t exclude_regs = (1 << dest_reg);
    ScratchRegAlloc src1_alloc = {0};
    ScratchRegAlloc src2_alloc = {0};
    ScratchRegAlloc scratch_alloc = {0};

    /* Handle constant operands */
    if (th_has_immediate_value(op->src1.r))
    {
      src1_alloc = get_scratch_reg_with_save(exclude_regs);
      src1_reg = src1_alloc.reg;
      exclude_regs |= (1 << src1_reg);
      load_to_reg(src1_reg, PREG_NONE, &op->src1);
    }
    else if (src1_reg >= 0)
    {
      exclude_regs |= (1 << src1_reg);
    }
    if (th_has_immediate_value(op->src2.r))
    {
      src2_alloc = get_scratch_reg_with_save(exclude_regs);
      src2_reg = src2_alloc.reg;
      exclude_regs |= (1 << src2_reg);
      load_to_reg(src2_reg, PREG_NONE, &op->src2);
    }
    else if (src2_reg >= 0)
    {
      exclude_regs |= (1 << src2_reg);
    }

    /* Get scratch register for quotient */
    scratch_alloc = get_scratch_reg_with_save(exclude_regs);
    int scratch = scratch_alloc.reg;

    /* quotient = dividend / divisor (unsigned) */
    ot_check(th_udiv(scratch, src1_reg, src2_reg));
    /* quotient = quotient * divisor */
    ot_check(th_mul(scratch, scratch, src2_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    /* result = dividend - quotient */
    ot_check(th_sub_reg(dest_reg, src1_reg, scratch, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                        ENFORCE_ENCODING_NONE));

    /* Restore allocated scratches in reverse order */
    restore_scratch_reg(&scratch_alloc);
    if (src2_alloc.reg != 0)
      restore_scratch_reg(&src2_alloc);
    if (src1_alloc.reg != 0)
      restore_scratch_reg(&src1_alloc);
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
  {
    int src_reg = op->src1.pr0;
    ScratchRegAlloc src_alloc = {0};

    /* Handle immediate constant - load into scratch register first */
    if (th_has_immediate_value(op->src1.r) || src_reg == PREG_NONE || (src_reg & PREG_SPILLED))
    {
      src_alloc = get_scratch_reg_with_save(0);
      src_reg = src_alloc.reg;
      load_to_reg(src_reg, PREG_NONE, &op->src1);
    }
    ot_check(th_cmp_imm(0, src_reg, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));

    /* Restore if allocated */
    if (src_alloc.reg != 0)
      restore_scratch_reg(&src_alloc);
    return;
  }
  default:
  {
    printf("compiler_error: unhandled data processing op: %s\n", tcc_ir_get_op_name(op->op));
  }
  }

  /* Handle constant operands - load into scratch registers if needed */
  {
    int src1_reg = op->src1.pr0;
    int src2_reg = op->src2.pr0;
    int src1_is_imm = th_has_immediate_value(op->src1.r);
    int src2_is_imm = th_has_immediate_value(op->src2.r);
    /* Check if src1 is VT_LOCAL address-of (needs address computation) */
    int src1_is_address_of = ((op->src1.r & VT_VALMASK) == VT_LOCAL) && !(op->src1.r & VT_LVAL);
    /* Check if src2 is VT_LOCAL address-of (needs address computation) */
    int src2_is_address_of = ((op->src2.r & VT_VALMASK) == VT_LOCAL) && !(op->src2.r & VT_LVAL);
    /* Check if src1 needs loading: immediate, address-of, or invalid/spilled register */
    int src1_needs_load = src1_is_imm || src1_is_address_of || src1_reg == PREG_NONE || (src1_reg & PREG_SPILLED);
    /* Check if src2 needs loading: immediate, address-of, or invalid/spilled register */
    int src2_needs_load = src2_is_imm || src2_is_address_of || src2_reg == PREG_NONE || (src2_reg & PREG_SPILLED);
    uint32_t exclude_regs = 0;
    ScratchRegAlloc src1_alloc = {0};
    ScratchRegAlloc src2_alloc = {0};
    ScratchRegAlloc dest_alloc = {0};

    /* Exclude destination register from scratch selection */
    if (op->dest.pr0 != PREG_NONE && !(op->dest.pr0 & PREG_SPILLED))
      exclude_regs |= (1 << op->dest.pr0);

    /* If src2 is already in a register, exclude it too so src1 doesn't clobber it */
    if (!src2_is_imm && !src2_is_address_of && src2_reg != PREG_NONE && !(src2_reg & PREG_SPILLED) && src2_reg < 16)
    {
      exclude_regs |= (1 << src2_reg);
    }

    /* Load src1 into scratch register if needed (immediate, VT_LOCAL address, spilled, etc.) */
    if (src1_needs_load)
    {
      src1_alloc = get_scratch_reg_with_save(exclude_regs);
      src1_reg = src1_alloc.reg;
      exclude_regs |= (1 << src1_reg);
      load_to_reg(src1_reg, PREG_NONE, &op->src1);
    }
    else if (src1_reg >= 0 && src1_reg < 16)
    {
      exclude_regs |= (1 << src1_reg);
    }

    /* Check if destination is in memory */
    int dest_reg = op->dest.pr0;
    int dest_is_memory = (dest_reg == PREG_NONE) || (dest_reg & PREG_SPILLED);

    if (src2_is_imm)
    {
      /* Try immediate form first (only if src1 didn't need loading from immediate and dest is in register) */
      if (!src1_is_imm && !dest_is_memory && handler.imm_handler &&
          ot(handler.imm_handler(dest_reg, src1_reg, op->src2.c.i, flags, ENFORCE_ENCODING_NONE)))
      {
        /* Success - clean up any allocated src1 scratch before returning */
        if (src1_alloc.reg != 0)
          restore_scratch_reg(&src1_alloc);
        return;
      }
      /* Immediate form failed or not available, load to scratch register */
      src2_alloc = get_scratch_reg_with_save(exclude_regs);
      src2_reg = src2_alloc.reg;
      load_to_reg(src2_reg, PREG_NONE, &op->src2);
    }
    else if (src2_needs_load)
    {
      /* src2 is not immediate but needs loading (VT_LOCAL address, spilled, etc.) */
      src2_alloc = get_scratch_reg_with_save(exclude_regs);
      src2_reg = src2_alloc.reg;
      load_to_reg(src2_reg, PREG_NONE, &op->src2);
    }

    /* Handle memory destination: allocate scratch register, perform op, store result */
    if (dest_is_memory)
    {
      /* Destination is in memory - need scratch register */
      if (src1_reg >= 0 && src1_reg < 16)
        exclude_regs |= (1 << src1_reg);
      if (src2_reg >= 0 && src2_reg < 16)
        exclude_regs |= (1 << src2_reg);
      dest_alloc = get_scratch_reg_with_save(exclude_regs);
      dest_reg = dest_alloc.reg;
    }

    ot_check(handler.reg_handler(dest_reg, src1_reg, src2_reg, flags, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));

    /* Store result to memory if needed */
    if (dest_is_memory)
    {
      store(dest_reg, &op->dest);
      restore_scratch_reg(&dest_alloc);
    }

    /* Restore allocated source scratches in reverse order */
    if (src2_alloc.reg != 0)
      restore_scratch_reg(&src2_alloc);
    if (src1_alloc.reg != 0)
      restore_scratch_reg(&src1_alloc);
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
  if (sv->pr0 != PREG_NONE && LS_IS_VFP_REG(sv->pr0))
  {
    return LS_VFP_REG_NUM(sv->pr0);
  }

  /* Not in VFP reg - load to integer reg and move to VFP scratch */
  load_to_reg(R0, is_double ? R1 : PREG_NONE, sv);
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
  if (dest->pr0 != PREG_NONE && LS_IS_VFP_REG(dest->pr0))
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
    if (dest->pr0 != PREG_NONE && !LS_IS_VFP_REG(dest->pr0) && !(dest->pr0 & PREG_SPILLED) && dest->pr1 != PREG_NONE)
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
    if (dest->pr0 != PREG_NONE && !LS_IS_VFP_REG(dest->pr0) && !(dest->pr0 & PREG_SPILLED))
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
  load_to_reg(R0, PREG_NONE, &q->src1);
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
    load_to_reg(R0, is_double ? R1 : PREG_NONE, &q->src1);
    /* Load 0x80000000 to scratch register using literal pool */
    ScratchRegAlloc scratch_alloc = get_scratch_reg_with_save((1 << R0) | (is_double ? (1 << R1) : 0));
    int scratch_reg = scratch_alloc.reg;
    load_full_const(scratch_reg, PREG_NONE, 0x80000000, NULL);
    if (is_double)
    {
      /* XOR high word (R1) with sign bit */
      ot_check(
          th_eor_reg(R1, R1, scratch_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    else
    {
      /* XOR R0 with sign bit */
      ot_check(
          th_eor_reg(R0, R0, scratch_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    restore_scratch_reg(&scratch_alloc);
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
      load_to_reg(R0, R1, &q->src1);
      load_to_reg(R2, R3, &q->src2);
    }
    else
    {
      /* Float: src1 in R0, src2 in R1 */
      load_to_reg(R0, PREG_NONE, &q->src1);
      load_to_reg(R1, PREG_NONE, &q->src2);
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
      load_to_reg(R0, src_is_double ? R1 : PREG_NONE, &q->src1);
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

  /* Constants are not held in a physical register; always materialize them
   * into the return registers, regardless of any (possibly stale) pr0/pr1
   * fields. */
  if ((q->src1.r & VT_VALMASK) == VT_CONST)
  {
    SValue dest;
    dest.pr0 = R0;
    dest.pr1 = is_64bit ? R1 : PREG_NONE;
    return load_to_dest(&dest, &q->src1);
  }

  /* NOTE: src1 is preloaded to a valid register by generate_code if it was spilled.
   * Just move to return registers R0 (and R1 for 64-bit). */
  if (q->src1.pr0 != PREG_NONE)
  {
    load_to_register(R0, q->src1.pr0, &q->src1);
    if (is_64bit && q->src1.pr1 != PREG_NONE)
    {
      load_to_register(R1, q->src1.pr1, &q->src1);
    }
    return;
  }

  /* If we get here with invalid pr0, handle constant case */
  SValue dest;
  dest.pr0 = R0;
  dest.pr1 = is_64bit ? R1 : PREG_NONE;
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

  src_reg = op->src1.pr0;
  ScratchRegAlloc scratch_alloc = {0};
  // if src_reg is PREG_NONE then immediate value must be loaded
  if (src_reg == PREG_NONE || (src_reg & PREG_SPILLED))
  {
    scratch_alloc = get_scratch_reg_with_save(0);
    src_reg = scratch_alloc.reg;
    load_to_reg(src_reg, is_64bit ? R11 : PREG_NONE, &op->src1);
  }
  store(src_reg, &op->dest);
  if (scratch_alloc.saved || scratch_alloc.reg >= 0)
    restore_scratch_reg(&scratch_alloc);
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
  thumb_gen_state.cached_global_reg = PREG_NONE;

  if (!leaffunc)
  {
    registers_to_push |= (1 << R_LR);
    registers_count++;
  }

  /* Keep FP whenever the function needs any FP-relative stack accesses.
   * The IR layer sets `need_frame_pointer` when parameters are passed on the
   * caller stack; locals/spills imply `stack_size > 0`. Don't clobber that
   * signal here.
   */
  {
    const int need_fp = (tcc_state->force_frame_pointer || tcc_state->need_frame_pointer || (stack_size > 0));
    tcc_state->need_frame_pointer = need_fp;
    if (need_fp)
    {
      registers_to_push |= (1 << R_FP);
      registers_count++;
    }
  }

  for (int i = R4; i <= R11; ++i)
  {
    if (tcc_state->text_and_data_separation && i == R9)
      continue;
    if (i == R_FP)
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
  /* Keep SP 8-byte aligned (AAPCS). tccir normally pre-aligns stack_size, but
   * be defensive here because other codepaths may call into the backend.
   * This also ensures call-sites that assume aligned SP remain correct.
   */
  if (stack_size & 7)
    stack_size = (stack_size + 7) & ~7;
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
    /* Parameter shuffling must not clobber still-needed incoming registers.
     * Example (4 args): if z is assigned to R3 and w is assigned to R6, a naive
     * sequence "mov r3,r2; mov r6,r3" destroys w.
     *
     * Strategy:
     * 1) Handle register-passed params first: store spills, collect reg->reg moves.
     * 2) Execute reg->reg moves as a parallel move with cycle breaking.
     * 3) Then load stack-passed params into their allocated registers.
     */

    typedef struct ParamMove
    {
      int dst;
      int src;
    } ParamMove;

    typedef struct StackParamLoad
    {
      int dst0;
      int dst1;
      int caller_off;
      int is_64bit;
    } StackParamLoad;

    /* NOTE: Do not hard-code small fixed arrays here.
     * Functions can legally have >32 parameters (e.g. sum40 in tests), and
     * overflowing these buffers corrupts prolog codegen and breaks calls.
     * Worst-case: a 64-bit param can contribute up to 2 reg moves.
     */
    const int max_param_moves = ir->next_parameter * 2 + 8;
    const int max_param_loads = ir->next_parameter + 8;
    ParamMove *moves = tcc_malloc(sizeof(ParamMove) * max_param_moves);
    int move_count = 0;
    StackParamLoad *loads = tcc_malloc(sizeof(StackParamLoad) * max_param_loads);
    int load_count = 0;

    for (int vreg = 0; vreg < ir->next_parameter; ++vreg)
    {
      const int encoded_vreg = (TCCIR_VREG_TYPE_PARAM << 28) | vreg;
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, encoded_vreg);

      if (!interval)
        continue;

      const int incoming_r0 = interval->incoming_reg0;
      const int incoming_r1 = interval->incoming_reg1;
      const int alloc_r0 = interval->allocation.r0;
      const int alloc_r1 = interval->allocation.r1;
      const int is_64bit = interval->is_double || interval->is_llong;

      if (incoming_r0 < 0)
      {
        /* Stack-passed parameter: defer loads until after register shuffles. */
        if (alloc_r0 != PREG_SPILLED && alloc_r0 != PREG_NONE && alloc_r0 >= 0 && alloc_r0 <= R12 &&
            interval->allocation.offset == 0)
        {
          const int caller_stack_offset = offset_to_args + interval->original_offset;
          loads[load_count++] = (StackParamLoad){
              .dst0 = alloc_r0,
              .dst1 = alloc_r1,
              .caller_off = caller_stack_offset,
              .is_64bit = is_64bit,
          };
        }
        else if (alloc_r0 == PREG_SPILLED || interval->allocation.offset != 0)
        {
          /* Stack-passed parameter that is also spilled: load from caller's stack
           * and store to our local stack (spill location). Use a scratch register. */
          const int caller_stack_offset = offset_to_args + interval->original_offset;
          const int spill_offset = interval->allocation.offset;
          int scratch = R_IP; /* Use IP as scratch */

          if (is_64bit)
          {
            /* Load low word from caller's stack */
            tcc_gen_machine_load_from_stack(scratch, caller_stack_offset);
            /* Store to spill location */
            tcc_gen_machine_store_to_stack(scratch, spill_offset);
            /* Load high word from caller's stack */
            tcc_gen_machine_load_from_stack(scratch, caller_stack_offset + 4);
            /* Store to spill location */
            tcc_gen_machine_store_to_stack(scratch, spill_offset + 4);
          }
          else
          {
            /* Load from caller's stack */
            tcc_gen_machine_load_from_stack(scratch, caller_stack_offset);
            /* Store to spill location */
            tcc_gen_machine_store_to_stack(scratch, spill_offset);
          }
        }
        continue;
      }

      /* Spilled parameters: store incoming regs to their stack slots. */
      if (alloc_r0 == PREG_SPILLED || interval->allocation.offset != 0)
      {
        const int stack_offset = interval->allocation.offset;
        if (is_64bit && incoming_r1 >= 0)
        {
          tcc_gen_machine_store_to_stack(incoming_r0, stack_offset);
          tcc_gen_machine_store_to_stack(incoming_r1, stack_offset + 4);
        }
        else
        {
          tcc_gen_machine_store_to_stack(incoming_r0, stack_offset);
        }
        continue;
      }

      /* Register-allocated parameters: record reg->reg moves (parallel move). */
      if (alloc_r0 != PREG_NONE && alloc_r0 >= 0 && alloc_r0 <= R12 && alloc_r0 != incoming_r0)
      {
        moves[move_count++] = (ParamMove){.dst = alloc_r0, .src = incoming_r0};
      }
      if (is_64bit && incoming_r1 >= 0 && alloc_r1 != PREG_NONE && alloc_r1 >= 0 && alloc_r1 <= R12 &&
          alloc_r1 != incoming_r1)
      {
        moves[move_count++] = (ParamMove){.dst = alloc_r1, .src = incoming_r1};
      }
    }

    /* Execute collected register moves with cycle breaking.
     * Greedy algorithm: emit moves whose source is not a destination; if only
     * cycles remain, spill one source into a temp register and continue.
     */
    while (move_count > 0)
    {
      uint32_t dst_mask = 0;
      for (int i = 0; i < move_count; ++i)
      {
        if (moves[i].dst >= 0 && moves[i].dst < 32)
          dst_mask |= (1u << moves[i].dst);
      }

      int progressed = 0;
      for (int i = 0; i < move_count; ++i)
      {
        const int dst = moves[i].dst;
        const int src = moves[i].src;
        if (dst == src)
        {
          moves[i] = moves[--move_count];
          --i;
          progressed = 1;
          continue;
        }
        if (src >= 0 && src < 32 && (dst_mask & (1u << src)))
          continue; /* src will be overwritten later */

        ot_check(
            th_mov_reg(dst, src, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
        moves[i] = moves[--move_count];
        --i;
        progressed = 1;
      }

      if (progressed)
        continue;

      /* Cycle: break it using a temporary register (prefer IP). */
      int temp = R_IP;
      if (dst_mask & (1u << temp))
      {
        /* Find any non-destination temp among allocatable regs. */
        for (int r = R4; r <= R11; ++r)
        {
          if (!(dst_mask & (1u << r)))
          {
            temp = r;
            break;
          }
        }
      }

      /* Save the source of the first move into temp, then rewrite that move
       * to read from temp; this makes it schedulable in the next iteration. */
      ot_check(th_mov_reg(temp, moves[0].src, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                          false));
      moves[0].src = temp;
    }

    /* Finally, load stack-passed parameters into their allocated registers. */
    for (int i = 0; i < load_count; ++i)
    {
      if (loads[i].is_64bit && loads[i].dst1 >= 0)
      {
        tcc_gen_machine_load_from_stack(loads[i].dst0, loads[i].caller_off);
        tcc_gen_machine_load_from_stack(loads[i].dst1, loads[i].caller_off + 4);
      }
      else
      {
        tcc_gen_machine_load_from_stack(loads[i].dst0, loads[i].caller_off);
      }
    }

    tcc_free(moves);
    tcc_free(loads);
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

  if (lr_saved)
  {
    pushed_registers |= 1 << R_PC;
    pushed_registers &= ~(1 << R_LR);
    ot_check(th_pop(pushed_registers));
    thumb_gen_state.generating_function = 0;
    th_literal_pool_generate();

    return;
  }
  if (pushed_registers > 0)
  {
    ot_check(th_pop(pushed_registers));
  }
  thumb_gen_state.generating_function = 0;
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

  /* NOTE: Avoid noisy debug prints in normal builds. */

  /* NOTE: Spilled destination handling is now done centrally in generate_code
   * via tcc_ir_storeback_spill. src1 is also preloaded if it was spilled. */

  if (is_64bit)
  {
    /* 64-bit assign/move must preserve both low and high words.
     * This is critical for switch-range lowering which spills 64-bit
     * temporaries to the stack and later reloads them for __aeabi_lcmp.
     */
    /* Only treat true lvalues as memory destinations here.
     * Spilled vregs are handled centrally via tcc_ir_preload_spills + tcc_ir_storeback_spill.
     */
    const int dest_in_mem = (op->dest.r & VT_LVAL) != 0;

    int src_lo = op->src1.pr0;
    int src_hi = op->src1.pr1;
    ScratchRegAlloc src_lo_alloc = {0};
    ScratchRegAlloc src_hi_alloc = {0};

    /* Materialize source into registers if needed (const/spilled/lvalue/etc). */
    if ((op->src1.r & VT_VALMASK) == VT_CONST || (op->src1.r & VT_LVAL) || src_lo == PREG_NONE ||
        (src_lo & PREG_SPILLED))
    {
      uint32_t exclude = 0;
      if (!dest_in_mem)
      {
        if (op->dest.pr0 != PREG_NONE && op->dest.pr0 <= 15)
          exclude |= (1u << op->dest.pr0);
        if (op->dest.pr1 != PREG_NONE && op->dest.pr1 <= 15)
          exclude |= (1u << op->dest.pr1);
      }
      src_lo_alloc = get_scratch_reg_with_save(exclude);
      exclude |= (1u << src_lo_alloc.reg);
      src_hi_alloc = get_scratch_reg_with_save(exclude);
      load_to_reg(src_lo_alloc.reg, src_hi_alloc.reg, &op->src1);
      src_lo = src_lo_alloc.reg;
      src_hi = src_hi_alloc.reg;
    }
    else if (src_hi == PREG_NONE || (src_hi & PREG_SPILLED))
    {
      /* Mixed 32->64 promotion: treat missing high word as 0. */
      uint32_t exclude = 0;
      if (!dest_in_mem)
      {
        if (op->dest.pr0 != PREG_NONE && op->dest.pr0 <= 15)
          exclude |= (1u << op->dest.pr0);
        if (op->dest.pr1 != PREG_NONE && op->dest.pr1 <= 15)
          exclude |= (1u << op->dest.pr1);
      }
      if (src_lo != PREG_NONE && src_lo <= 15)
        exclude |= (1u << src_lo);
      src_hi_alloc = get_scratch_reg_with_save(exclude);
      ot_check(th_mov_imm(src_hi_alloc.reg, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      src_hi = src_hi_alloc.reg;
    }

    if (dest_in_mem)
    {
      /* Store low and high words separately as 32-bit stores. */
      SValue dest_low = op->dest;
      SValue dest_high = op->dest;
      dest_low.type.t = (dest_low.type.t & ~VT_BTYPE) | (VT_INT | (dest_low.type.t & VT_UNSIGNED));
      dest_high.type.t = dest_low.type.t;
      dest_high.c.i += 4;

      store(src_lo, &dest_low);
      store(src_hi, &dest_high);
    }
    else
    {
      if (op->dest.pr0 != src_lo)
      {
        ot_check(th_mov_reg(op->dest.pr0, src_lo, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE, false));
      }
      if (op->dest.pr1 != src_hi)
      {
        ot_check(th_mov_reg(op->dest.pr1, src_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE, false));
      }
    }

    restore_scratch_reg(&src_hi_alloc);
    restore_scratch_reg(&src_lo_alloc);
    return;
  }

  if ((op->src1.r & VT_VALMASK) == VT_CONST)
  {
    if (dest_is_vfp)
    {
      int dn = LS_VFP_REG_NUM(op->dest.pr0);
      /* Load constant to integer register, then move to VFP */
      ScratchRegAlloc scratch_alloc = get_scratch_reg_with_save(0);
      int scratch_reg = scratch_alloc.reg;
      load_to_reg(scratch_reg, PREG_NONE, &op->src1);
      ot_check(th_vmov_gp_sp(scratch_reg, dn, 0)); /* VMOV Sn, scratch_reg */
      restore_scratch_reg(&scratch_alloc);
    }
    else if ((op->dest.r & VT_LVAL) && ((op->dest.r & VT_VALMASK) == VT_LOCAL || (op->dest.r & VT_VALMASK) == VT_CONST))
    {
      /* Destination is a memory location (e.g., spilled variable with address taken).
       * Load constant into scratch register, then store to destination memory.
       * Remove VT_LVAL to prevent store() from trying to dereference. */
      ScratchRegAlloc scratch_alloc = get_scratch_reg_with_save(0);
      int scratch_reg = scratch_alloc.reg;
      load_to_reg(scratch_reg, PREG_NONE, &op->src1);
      SValue dest_direct = op->dest;
      dest_direct.r &= ~VT_LVAL; /* Clear VT_LVAL - we want direct store to stack offset */
      store(scratch_reg, &dest_direct);
      restore_scratch_reg(&scratch_alloc);
    }
    else
    {
      load_to_dest(&op->dest, &op->src1);
    }
    return;
  }

  /* Handle VT_LOCAL - this is address-of spilled vreg.
   * The src1.r will be VT_LOCAL without VT_LVAL (address computation, not value load). */
  if ((op->src1.r & VT_VALMASK) == VT_LOCAL)
  {
    load_to_dest(&op->dest, &op->src1);
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

  /* NOTE: Writeback to memory for spilled destinations is handled by tcc_ir_storeback_spill
   * in generate_code(). We should NOT do an inline writeback here because:
   * 1. Spilled temporaries (TMP vregs) are handled by storeback_spill
   * 2. For actual local variables (VAR vregs) that are in registers, the register IS the
   *    canonical location - no writeback needed.
   * 3. Double writeback causes bugs when VT_LVAL is set (would store TO address instead of AT address)
   */
}

/* Load Effective Address: compute the address of src1 into dest.
 * This is the explicit "address-of" operation for local variables/arrays.
 * Unlike LOAD which dereferences, LEA computes FP+offset into a register.
 */
ST_FUNC void tcc_gen_machine_lea_op(TACQuadruple *op)
{
  int dest_reg = op->dest.pr0;
  int src_v = op->src1.r & VT_VALMASK;

  /* Handle spilled destination - use scratch register then store */
  ScratchRegAlloc dest_alloc = {0};
  if (dest_reg == PREG_NONE || (dest_reg & PREG_SPILLED))
  {
    dest_alloc = get_scratch_reg_with_save(0);
    dest_reg = dest_alloc.reg;
  }

  if (src_v == VT_LOCAL || src_v == VT_LLOCAL)
  {
    /* Compute address of local: FP + offset */
    int base = R_FP;
    if (tcc_state->need_frame_pointer == 0)
      base = R_SP;

    int offset = (int)op->src1.c.i;
    int sign = (offset < 0);
    int abs_offset = sign ? -offset : offset;

    if (sign)
    {
      /* SUB dest, base, #offset */
      if (!ot(th_sub_imm(dest_reg, base, abs_offset, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)))
      {
        /* Large offset: load into scratch and subtract */
        ScratchRegAlloc scratch = get_scratch_reg_with_save((1u << dest_reg) | (1u << base));
        load_full_const(scratch.reg, PREG_NONE, abs_offset, NULL);
        ot_check(th_sub_reg(dest_reg, base, scratch.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&scratch);
      }
    }
    else
    {
      /* ADD dest, base, #offset */
      if (!ot(th_add_imm(dest_reg, base, abs_offset, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)))
      {
        /* Large offset: load into scratch and add */
        ScratchRegAlloc scratch = get_scratch_reg_with_save((1u << dest_reg) | (1u << base));
        load_full_const(scratch.reg, PREG_NONE, abs_offset, NULL);
        ot_check(th_add_reg(dest_reg, base, scratch.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&scratch);
      }
    }
  }
  else if (src_v == VT_CONST && (op->src1.r & VT_SYM))
  {
    /* Address of global symbol */
    load_full_const(dest_reg, PREG_NONE, op->src1.c.i, op->src1.sym);
  }
  else
  {
    /* Fallback: if src is already in a register, just move it */
    int src_reg = op->src1.pr0;
    if (src_reg != PREG_NONE && !(src_reg & PREG_SPILLED) && src_reg != dest_reg)
    {
      ot_check(th_mov_reg(dest_reg, src_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                          false));
    }
    else if (src_reg == PREG_NONE || (src_reg & PREG_SPILLED))
    {
      tcc_error("compiler_error: LEA on unexpected operand type r=0x%x", op->src1.r);
    }
  }

  /* Store back if destination was spilled */
  if (dest_alloc.reg != 0)
  {
    if ((op->dest.pr0 & PREG_SPILLED) && op->dest.c.i != 0)
    {
      /* Store to spill slot */
      int offset = (int)op->dest.c.i;
      int sign = (offset < 0);
      int abs_offset = sign ? -offset : offset;
      if (!store_word_to_base(dest_reg, R_FP, abs_offset, sign))
      {
        ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(abs_offset, sign, (1u << dest_reg) | (1u << R_FP));
        int rr = rr_alloc.reg;
        ot_check(th_str_reg(dest_reg, R_FP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&rr_alloc);
      }
    }
    restore_scratch_reg(&dest_alloc);
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
    /* Don't reuse the source register as offset scratch, otherwise we'd
     * clobber the value before the STR (e.g. store -offset instead of value). */
    ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(abs_offset, sign, (1u << reg) | (1u << R_FP));
    int rr = rr_alloc.reg;
    ot_check(th_str_reg(reg, R_FP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&rr_alloc);
  }
}

/* Load a register from a stack slot relative to FP.
 * Used for stack-passed incoming parameters (caller stack is above FP). */
static void tcc_gen_machine_load_from_stack(int reg, int offset)
{
  int sign = (offset < 0);
  int abs_offset = sign ? -offset : offset;

  /* Try direct LDR with immediate offset */
  if (!load_word_from_base(reg, R_FP, abs_offset, sign))
  {
    /* Offset too large, use scratch register */
    /* Avoid using the destination register as offset scratch; some Thumb
     * encodings have unpredictable behavior when Rt == Rm. */
    ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(abs_offset, sign, (1u << reg) | (1u << R_FP));
    int rr = rr_alloc.reg;
    ot_check(th_ldr_reg(reg, R_FP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&rr_alloc);
  }
}

static void gcall_or_jump(int is_jmp, SValue *dest)
{
  if ((dest->r & (VT_VALMASK | VT_LVAL)) == VT_CONST)
  {
    /* IMPORTANT: ot_check() may flush a pending literal pool *before* emitting
     * this BL, which inserts a pool skip-branch at the current `ind`.
     * If we record the relocation at `ind` before ot_check(), the linker will
     * patch the pool skip-branch instead of the BL (corrupting control flow).
     *
     * Therefore: emit first, then record relocation at the actual BL position.
     */
    uint32_t imm;
    if (dest->r & VT_SYM)
    {
      /* For symbol relocations, keep a benign placeholder immediate.
       * Using -4 encodes a self-call (common placeholder) and provides a
       * stable addend independent of any pool flush.
       */
      imm = (uint32_t)-4;
    }
    else
    {
      imm = th_encbranch(ind, ind + dest->c.i);
    }

    TRACE("gcall_or_jmp: %d, ind: 0x%x, 0x%x", is_jmp, ind, imm);
    if (imm)
    {
      ot_check(th_bl_t1(imm));
      if (dest->r & VT_SYM)
      {
        int call_pos = ind - 4; /* th_bl_t1 is always 4 bytes */
        greloc(cur_text_section, dest->sym, call_pos, R_ARM_THM_JUMP24);
      }
    }
  }
  else
  {
    /* Treat both VT_FUNC and pointer-to-function as function designators.
     * If we already have the address in a register, it must be used directly
     * as the branch target (not loaded from). */
    int bt = dest->type.t & VT_BTYPE;
    int is_func_designator = 0;
    if (bt == VT_FUNC)
      is_func_designator = 1;
    else if (bt == VT_PTR && dest->type.ref)
    {
      int ref_bt = dest->type.ref->type.t & VT_BTYPE;
      if (ref_bt == VT_FUNC)
        is_func_designator = 1;
    }

    /* Calling through a function pointer may involve an explicit or implicit
     * dereference (e.g. (*fp)()). In C this yields a *function designator* at
     * the address held in the pointer; it is NOT a memory location that should
     * be loaded from.
     *
     * If the target type is VT_FUNC and the address already lives in a register
     * (v < VT_CONST), clear VT_LVAL so we don't emit a bogus extra load like
     *   ldr ip, [ip]
     * before blx.
     */
    if (is_func_designator)
    {
      int v = dest->r & VT_VALMASK;
      if ((dest->r & VT_LVAL) && v < VT_CONST)
        dest->r &= ~VT_LVAL;
    }

    /* Indirect call/jump: keep argument registers (R0-R3) intact.
     * In particular, for indirect calls the target must NOT live in R0,
     * otherwise arg0 gets overwritten (e.g. fprintfptr(stdout, ...)).
     * Prefer R12/IP which is caller-saved by the ABI.
     */
    if (is_jmp)
    {
      load_to_reg(R_IP, PREG_NONE, dest);
      ot_check(th_bx_reg(R_IP));
    }
    else
    {
      ScratchRegAlloc scratch = get_scratch_reg_with_save((1u << R0) | (1u << R1) | (1u << R2) | (1u << R3));

      /* Keep argument registers off-limits while materializing the target. */
      uint32_t old_exclude = scratch_global_exclude;
      scratch_global_exclude |= (1u << R0) | (1u << R1) | (1u << R2) | (1u << R3);

      load_to_reg(scratch.reg, PREG_NONE, dest);

      scratch_global_exclude = old_exclude;
      ot_check(th_blx_reg(scratch.reg));
      restore_scratch_reg(&scratch);
    }
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
    /* VT_LOCAL without VT_LVAL means we need the ADDRESS of the local variable.
     * In this case we must compute FP + offset, not do a register move. */
    if (!(sv->r & VT_LVAL))
    {
      /* Always compute address via load_vt_local */
      int r1 = (sv->pr1 != PREG_NONE && is_64bit_type(sv->type.t)) ? sv->pr1 : PREG_NONE;
      load_to_reg(reg, r1, sv);
      return;
    }
    if (sv->pr0 != PREG_NONE && !(sv->pr0 & PREG_SPILLED))
    {
      // load local variable value from register
      if (reg != reg_from)
      {
        ot_check(th_mov_reg(reg, reg_from, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                            false));
      }
      return;
    }
    if (sv->pr0 == PREG_NONE || (sv->pr0 & PREG_SPILLED))
    {
      /* Spilled local variable - load from stack */
      int r1 = (sv->pr1 != PREG_NONE && is_64bit_type(sv->type.t)) ? sv->pr1 : PREG_NONE;
      load_to_reg(reg, r1, sv);
      return;
    }
  }

  if ((sv->r & VT_LVAL) || sv->pr0 == PREG_NONE || (sv->pr0 & PREG_SPILLED))
  {
    /* Lvalue: need to load from memory */
    int r1 = (sv->pr1 != PREG_NONE && is_64bit_type(sv->type.t)) ? sv->pr1 : PREG_NONE;
    load_to_reg(reg, r1, sv);
    return;
  }

  /* Value is in a valid register - move it.
   * For 64-bit values, callers may request moving either the low or high word
   * via 'reg_from'. Using sv->pr0 unconditionally breaks word selection and
   * duplicates the low word into the high word (seen in 118_switch.c).
   */
  int src_reg = (reg_from != PREG_NONE) ? reg_from : sv->pr0;
  if (src_reg == PREG_NONE || (src_reg & PREG_SPILLED))
  {
    int r1 = (sv->pr1 != PREG_NONE && is_64bit_type(sv->type.t)) ? sv->pr1 : PREG_NONE;
    load_to_reg(reg, r1, sv);
    return;
  }
  if (reg != src_reg)
  {
    ot_check(
        th_mov_reg(reg, src_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
  }
}

/* Returns true when a register value can be used directly as a source (not spilled, not lvalue).
 * For VT_LOCAL without VT_LVAL (address of local variable), we need to compute the address,
 * so we cannot use the register directly even if it contains the value. */
static bool is_valid_src_reg(const SValue *sv, int reg)
{
  if (reg == PREG_NONE || (reg & PREG_SPILLED))
    return false;
  if (sv->r & VT_LVAL)
    return false;
  /* VT_LOCAL without VT_LVAL means "address of local variable" - needs address computation */
  if ((sv->r & VT_VALMASK) == VT_LOCAL)
    return false;
  return true;
}

static int lowest_set_bit(uint32_t mask)
{
  return __builtin_ctz(mask);
}

/* Preserve a register that will be clobbered by later parameter writes. We try to remap it to R12.
 * This also handles lvalue sources - if a param needs to dereference through a register,
 * remapping the pointer to R12 allows the load to happen from R12 instead. */
static void remap_future_param_sources(int current_dest, int reg_to_save, int remap_reg, int *param_src0,
                                       int *param_src1, int *param_lval_src, int *op_to_reg)
{
  /* Walk future params (those with lower destination registers) and rewrite their sources. */
  for (int dest = current_dest - 1; dest >= 0; --dest)
  {
    if (op_to_reg[dest] == PREG_NONE)
      continue;
    if (param_src0[dest] == reg_to_save)
      param_src0[dest] = remap_reg;
    if (param_src1[dest] == reg_to_save)
      param_src1[dest] = remap_reg;
    if (param_lval_src[dest] == reg_to_save)
      param_lval_src[dest] = remap_reg;
  }
}

ST_FUNC void tcc_gen_machine_func_call_op(TACQuadruple *q, int drop_result, TCCIRState *ir, int call_idx)
{
  /* IR owns call argument binding; backend must not scan for FUNCPARAM*. */
  const IRCallSite *cs = tcc_ir_callsite_for_call(ir, call_idx);
  int param_count = 0;
  const int *param_indices = NULL;
  if (cs)
  {
    param_count = cs->argc;
    param_indices = cs->arg_instr_index_by_num;
  }
  else
  {
    /* Should not happen: tcc_ir_generate_code() builds callsites upfront. */
    tcc_error("Missing callsite binding for call at IR index %d", call_idx);
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
  int op_to_reg[4] = {PREG_NONE, PREG_NONE, PREG_NONE, PREG_NONE}; /* Map of register to param index */
  int stack_offset = 0;

  // only r0-r3 for arguments, rest arguments go to stack
  for (int i = 0; i < param_count; ++i)
  {
    const int argument_index = param_indices[i];
    TACQuadruple *arg = &ir->instructions[argument_index];
    const int is_64bit = is_64bit_type(arg->src1.type.t);
    const int is_struct = (arg->src1.type.t & VT_BTYPE) == VT_STRUCT;
    int arg_size = is_64bit ? 8 : 4;

    /* For structs passed by value, use actual struct size */
    if (is_struct && !is_64bit)
    {
      int align;
      arg_size = type_size(&arg->src1.type, &align);
      arg_size = TCC_ALIGN(arg_size, 4); /* Round up to 4-byte alignment */
    }

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
      /* 32-bit value or struct */
      if (next_reg <= 3 && !is_struct)
      {
        /* Scalars fit in one register */
        register_map |= (1 << next_reg);
        op_to_reg[next_reg] = i;
        next_reg++;
      }
      else if (is_struct)
      {
        /* Structs may occupy multiple registers or go to stack */
        int regs_needed = (arg_size + 3) / 4; /* Number of 4-byte slots */
        if (next_reg + regs_needed <= 4)
        {
          /* Fits in remaining registers */
          for (int r = 0; r < regs_needed; r++)
          {
            register_map |= (1 << (next_reg + r));
            op_to_reg[next_reg + r] = i;
          }
          next_reg += regs_needed;
        }
        else
        {
          /* Goes to stack */
          stack_size = TCC_ALIGN(stack_size, 4);
          stack_size += arg_size;
          next_reg = 4; /* No more registers available */
        }
      }
      else
      {
        /* Scalar goes to stack */
        stack_size += 4;
      }
    }
  }

  /* Align total stack to 8 bytes as required by AAPCS */
  stack_size = TCC_ALIGN(stack_size, 8);

  /* Some configurations preserve additional registers around calls.
   * IMPORTANT: any such pushes must happen BEFORE laying out stack arguments,
   * otherwise SP at call-time no longer matches where we stored the args.
   * This breaks stack-passed args and variadic calls (e.g. printf in tests).
   */
  int registers_to_push = 0;
  if (tcc_state->text_and_data_separation && (q->src1.type.t & VT_EXTERN))
  {
    /* Preserve the cached-global registers across external calls.
     * Use bitwise OR (not logical OR).
     */
    registers_to_push |= (1 << R9) | (1 << R8);
  }
  if (registers_to_push != 0)
  {
    ot_check(th_push(registers_to_push));
  }

  /* Reserve stack space for arguments if needed */
  if (stack_size > 0)
  {
    ot_check(th_sub_sp_imm(R_SP, stack_size, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }

  /* Push stack arguments first */
  for (int i = 0; i < param_count; ++i)
  {
    // check if argument goes to register
    int assigned_register = PREG_NONE;
    int is_64bit = 0;
    const int argument_index = param_indices[i];
    TACQuadruple *arg = &ir->instructions[argument_index];

    for (int j = 0; j < 4; j++)
    {
      if (op_to_reg[j] == PREG_NONE)
        continue;
      if (op_to_reg[j] == i)
      {
        assigned_register = j;
        break;
      }
    }
    if (assigned_register != PREG_NONE)
    {
      continue;
    }

    is_64bit = is_64bit_type(arg->src1.type.t);
    if (is_64bit)
    {
      /* 64-bit stack arguments must be 8-byte aligned */
      stack_offset = TCC_ALIGN(stack_offset, 8);

      int reg_lo = arg->src1.pr0;
      int reg_hi = arg->src1.pr1;
      ScratchRegAlloc scratch_lo_alloc = {0};
      ScratchRegAlloc scratch_hi_alloc = {0};

      /* Load low and high parts into scratch registers if needed */
      if (reg_lo == PREG_NONE || (reg_lo & PREG_SPILLED) || (arg->src1.r & VT_LVAL))
      {
        /* Need to load the 64-bit value to registers first */
        /* Do not clobber argument registers (R0-R3): they may hold other args
         * or values that will be forwarded into registers later (e.g. long long
         * return value in R0/R1). */
        const uint32_t stack_exclude = (1u << R_SP) | (1u << R7) | (1u << R0) | (1u << R1) | (1u << R2) | (1u << R3);
        scratch_lo_alloc = get_scratch_reg_with_save(stack_exclude);
        reg_lo = scratch_lo_alloc.reg;
        scratch_hi_alloc = get_scratch_reg_with_save(stack_exclude | (1u << reg_lo));
        reg_hi = scratch_hi_alloc.reg;
        load_to_reg(reg_lo, reg_hi, &arg->src1);
      }

      /* Store low word first, then high word */
      ot_check(th_str_imm(reg_lo, R_SP, stack_offset, 6, ENFORCE_ENCODING_NONE));
      ot_check(th_str_imm(reg_hi, R_SP, stack_offset + 4, 6, ENFORCE_ENCODING_NONE));
      stack_offset += 8;

      /* Restore scratches after use */
      restore_scratch_reg(&scratch_hi_alloc);
      restore_scratch_reg(&scratch_lo_alloc);
    }
    else
    {
      int reg = arg->src1.pr0;
      ScratchRegAlloc scratch_alloc = {0};
      if (reg == PREG_NONE || (reg & PREG_SPILLED) || (arg->src1.r & VT_LVAL))
      {
        /* Avoid clobbering R0-R3 for the same reason as above. */
        const uint32_t stack_exclude = (1u << R_SP) | (1u << R7) | (1u << R0) | (1u << R1) | (1u << R2) | (1u << R3);
        scratch_alloc = get_scratch_reg_with_save(stack_exclude);
        reg = scratch_alloc.reg;
        load_to_reg(reg, PREG_NONE, &arg->src1);
      }
      ot_check(th_str_imm(reg, R_SP, stack_offset, 6, ENFORCE_ENCODING_NONE));
      stack_offset += 4;

      /* Restore scratch after use */
      restore_scratch_reg(&scratch_alloc);
    }
  }
  /* Pre-compute register sources for each register-assigned argument so we can spot conflicts.
   * For lvalues (VT_LVAL set), the pr0 register contains a pointer that will be dereferenced.
   * Even though we can't remap this (we must load through it), we still need to track that
   * this register will be READ from - so writing to it as a destination would be wrong.
   * Track lvalue source registers separately so we can detect these conflicts. */
  int param_src0[4] = {PREG_NONE, PREG_NONE, PREG_NONE, PREG_NONE};
  int param_src1[4] = {PREG_NONE, PREG_NONE, PREG_NONE, PREG_NONE};
  int param_lval_src[4] = {PREG_NONE, PREG_NONE, PREG_NONE, PREG_NONE}; /* Source reg for lvalue dereference */
  uint32_t future_src_mask = 0;
  for (int dest = 0; dest < 4; ++dest)
  {
    if (op_to_reg[dest] == PREG_NONE)
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
    else if ((arg->src1.r & VT_LVAL) && arg->src1.pr0 != PREG_NONE && !(arg->src1.pr0 & PREG_SPILLED))
    {
      /* Lvalue: pr0 contains pointer to dereference. Track it separately. */
      param_lval_src[dest] = arg->src1.pr0;
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
  /* If this is an indirect call and the call target currently lives in an
   * argument register (R0-R3), preserve it while materializing arguments.
   * Use an aligned temp stack slot and reload into IP right before BLX.
   * This avoids callee-saved register bookkeeping and keeps ABI stack
   * alignment valid at the call boundary. */
  const int orig_func_ptr_reg = q->src1.pr0;
  int spilled_call_target = 0;
  if (orig_func_ptr_reg >= R0 && orig_func_ptr_reg <= R3)
  {
    ot_check(th_sub_sp_imm(R_SP, 8, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    ot_check(th_str_imm(orig_func_ptr_reg, R_SP, 0, 6, ENFORCE_ENCODING_NONE));
    spilled_call_target = 1;
  }

  uint32_t call_target_mask = 0;

  /* Compute sources needed by each lower register before we start loading.
   * Include both direct register sources (param_src0/param_src1) and lvalue
   * pointer sources (param_lval_src) that will be dereferenced. */
  uint32_t sources_needed_by_lower[4] = {0, 0, 0, 0};
  for (int dest = 0; dest < 4; ++dest)
  {
    for (int lower = 0; lower < dest; ++lower)
    {
      if (param_src0[lower] != PREG_NONE)
        sources_needed_by_lower[dest] |= (1u << param_src0[lower]);
      if (param_src1[lower] != PREG_NONE)
        sources_needed_by_lower[dest] |= (1u << param_src1[lower]);
      if (param_lval_src[lower] != PREG_NONE)
        sources_needed_by_lower[dest] |= (1u << param_lval_src[lower]);
    }
  }

  /* While materializing call arguments, some helpers may allocate scratch
   * registers based on vreg liveness. The physical argument registers (R0-R3)
   * are not represented as live vregs here, so without extra care the scratch
   * allocator can (incorrectly) pick an already-prepared argument register and
   * clobber it.
   *
   * Protect all argument registers globally during argument setup, and
   * temporarily allow the specific destination register(s) we're writing.
   * Also protect R7 (frame pointer) which must not be used as scratch.
   */
  uint32_t saved_global_exclude = scratch_global_exclude;
  scratch_global_exclude |= (1u << R0) | (1u << R1) | (1u << R2) | (1u << R3) | (1u << R7) | call_target_mask;

  /* FIRST PASS: Process lvalue params in ASCENDING order (R0, R1, R2, R3).
   * For lvalue sources, the source register contains a pointer that we load through.
   * Processing in ascending order ensures we don't clobber a source register that
   * a higher-numbered param still needs. For example:
   *   R1 loads from [R2], R2 loads from [R3], R3 loads from [R4]
   * Processing R1 first (loading from R2) is safe because R2 will be written
   * AFTER R1, so R2's old value (the pointer) is still valid when R1 reads it.
   */
  uint32_t lval_params_done = 0;
  for (int i = 0; i <= 3; ++i)
  {
    if (op_to_reg[i] == PREG_NONE)
      continue;
    if (param_lval_src[i] == PREG_NONE)
      continue; /* Not an lvalue param, skip for now */

    /* For 64-bit args, only process the low register of the pair here.
     * We'll load both words into (Rn, Rn+1) in one go.
     */
    {
      TACQuadruple *arg_probe = &ir->instructions[param_indices[op_to_reg[i]]];
      if (is_64bit_type(arg_probe->src1.type.t) && (i & 1))
        continue;
    }

    /* If the current destination register will be overwritten by materializing
     * this lvalue, but its *current* value is still needed as a pointer source
     * for a higher-numbered lvalue argument, preserve it in IP (R12) and
     * remap those future lvalue loads to use IP.
     *
     * This prevents patterns like:
     *   R2 = *(...)        ; loads arg2, clobbers pointer-in-R2
     *   R3 = *R2           ; intended load through pointer, now wrong
     *
     * Seen in variadic calls where multiple args are loaded from memory.
     */
    {
      int need_preserve = 0;
      for (int higher = i + 1; higher <= 3; ++higher)
      {
        if (op_to_reg[higher] == PREG_NONE)
          continue;
        if (param_lval_src[higher] == i)
        {
          need_preserve = 1;
          break;
        }
      }

      if (need_preserve)
      {
        /* Only use IP if it isn't already a required source for some argument.
         * (If it is, we'd clobber that value.) */
        int ip_busy = 0;
        for (int r = 0; r < 4; ++r)
        {
          if (param_src0[r] == R_IP || param_src1[r] == R_IP || param_lval_src[r] == R_IP)
          {
            ip_busy = 1;
            break;
          }
        }

        if (!ip_busy)
        {
          ot_check(
              th_mov_reg(R_IP, i, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
          for (int higher = i + 1; higher <= 3; ++higher)
          {
            if (param_lval_src[higher] == i)
              param_lval_src[higher] = R_IP;
          }
        }
      }
    }

    TACQuadruple *arg = &ir->instructions[param_indices[op_to_reg[i]]];
    SValue arg_copy = arg->src1;
    const int is_64bit = is_64bit_type(arg_copy.type.t);
    const int dest_reg = i;

    /* Apply lvalue source remapping if any */
    arg_copy.pr0 = param_lval_src[dest_reg];

    const uint32_t tmp_global_exclude = scratch_global_exclude;
    const uint32_t dest_mask = is_64bit ? ((1u << dest_reg) | (1u << (dest_reg + 1))) : (1u << dest_reg);
    scratch_global_exclude &= ~dest_mask;

    if (is_64bit)
    {
      /* 64-bit lvalue - load both low/high words into the argument pair.
       * Previously we only loaded the low word and marked the pair as done,
       * leaving the high word uninitialized (breaks 118_switch.c).
       */
      if (dest_reg <= 2)
      {
        load_to_reg(dest_reg, dest_reg + 1, &arg_copy);
      }
      else
      {
        /* Should not happen (64-bit args are even-aligned), but be safe. */
        load_to_register(dest_reg, arg_copy.pr0, &arg_copy);
      }
    }
    else
    {
      /* 32-bit lvalue - load directly to destination register */
      load_to_register(dest_reg, arg_copy.pr0, &arg_copy);
    }

    scratch_global_exclude = tmp_global_exclude;
    lval_params_done |= dest_mask;
  }

  /* SECOND PASS: Process non-lvalue params in DESCENDING order (R3, R2, R1, R0).
   * This is the standard order that avoids clobbering source registers. */
  for (int i = 3; i >= 0; --i)
  {
    if (op_to_reg[i] == PREG_NONE)
      continue;
    if (lval_params_done & (1u << i))
      continue; /* Already handled in first pass */

    TACQuadruple *arg = &ir->instructions[param_indices[op_to_reg[i]]];
    SValue arg_copy = arg->src1; /* We may rewrite pr0/pr1 if we remap sources. */

    const int is_64bit = is_64bit_type(arg_copy.type.t);
    const int dest_reg = i;
    const uint32_t dest_mask = is_64bit ? ((1u << dest_reg) | (1u << (dest_reg - 1))) : (1u << dest_reg);

    /* Check if writing to dest_reg would clobber a source needed by lower registers.
     * Note: lvalue sources are now handled in the first pass, so we only need to
     * check non-lvalue sources here. */
    const uint32_t conflict_mask = dest_mask & sources_needed_by_lower[dest_reg];
    if (conflict_mask)
    {
      const int reg_to_save = lowest_set_bit(conflict_mask); /* pick lowest conflicting register */
      const bool r12_busy = sources_needed_by_lower[dest_reg] & (1u << R_IP);
      if (!r12_busy && !(dest_mask & (1u << R_IP)))
      {
        ot_check(th_mov_reg(R_IP, reg_to_save, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE, false));
        remap_future_param_sources(dest_reg, reg_to_save, R_IP, param_src0, param_src1, param_lval_src, op_to_reg);
      }
      else
      {
        /* No safe scratch register; skip remapping (rare). */
      }
    }

    /* Apply any remapping for this argument (non-lvalue sources only in this pass). */
    if (param_src0[dest_reg] != PREG_NONE)
      arg_copy.pr0 = param_src0[dest_reg];
    if (is_64bit && param_src1[dest_reg] != PREG_NONE)
      arg_copy.pr1 = param_src1[dest_reg];

    /* Allow the destination register(s) for this argument to be used by
     * address calculation helpers, but keep other arg registers protected. */
    const uint32_t tmp_global_exclude = scratch_global_exclude;
    scratch_global_exclude &= ~dest_mask;

    if (is_64bit)
    {
      /* 64-bit values use register pairs (R0:R1 or R2:R3) */
      SValue dest;
      dest.pr0 = dest_reg - 1;
      dest.pr1 = dest_reg;
      --i; /* Skip the lower register of the pair in next iteration */

      /* If either half isn't a usable register source (spilled, lvalue, etc),
       * materialize the whole 64-bit value into the destination pair.
       * This avoids trying to load only one half from memory and also keeps
       * the low/high word selection consistent.
       */
      const bool src0_ok = is_valid_src_reg(&arg_copy, arg_copy.pr0);
      const bool src1_ok = is_valid_src_reg(&arg_copy, arg_copy.pr1);
      if (!src0_ok || !src1_ok)
      {
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

    scratch_global_exclude = tmp_global_exclude;
  }

  scratch_global_exclude = saved_global_exclude;

  /* registers_to_push handled before stack arg layout */

  if (spilled_call_target)
  {
    /* Reload the indirect call target and perform the call directly.
     * Clear VT_LVAL to prevent accidental extra dereference. */
    q->src1.r &= ~VT_LVAL;
    ot_check(th_ldr_imm(R_IP, R_SP, 0, 6, ENFORCE_ENCODING_NONE));
    ot_check(th_add_sp_imm(R_SP, 8, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    ot_check(th_blx_reg(R_IP));
  }
  else
  {
    gcall_or_jump(0, &q->src1);
  }

  /* Invalidate global symbol cache after function call.
   * All caller-saved registers (R0-R3, R12, LR) are clobbered by the call,
   * so any cached global address in those registers is now invalid. */
  if (th_is_caller_saved_register(thumb_gen_state.cached_global_reg))
  {
    thumb_gen_state.cached_global_sym = NULL;
    thumb_gen_state.cached_global_reg = PREG_NONE;
  }

  /* Clean up stack space used for arguments (undo sub_sp before popping). */
  if (stack_size > 0)
  {
    ot_check(th_add_sp_imm(R_SP, stack_size, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }
  if (registers_to_push != 0)
  {
    ot_check(th_pop(registers_to_push));
  }

  if (drop_result)
  {
    /* param_indices is owned by IR callsite table */
    return;
  }

  /* Handle the return value - move from R0 (and R1 for 64-bit) to destination */
  int dest_spilled = (q->dest.pr0 == PREG_NONE) || (q->dest.pr0 & PREG_SPILLED);

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

  /* param_indices is owned by IR callsite table */
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
  /* Convert comparison flags to 0/1 value in destination register.
   * Keep the IT block to a single instruction so it cannot accidentally
   * cover a later instruction (e.g. return-value move), which would leave
   * the destination unchanged on the false path.
   *
   *   MOV Rd, #0          ; must NOT clobber flags
   *   IT <cond>
   *   MOV<cond> Rd, #1
   */
  int op = mapcc(q->src1.c.i);
  int dest = q->dest.pr0;

  /* NOTE: Destination is preloaded to a valid register by generate_code if spilled.
   * Just use it directly. Store-back is also handled centrally. */

  /* Ensure the default false result without touching flags (flags are the predicate input). */
  ot_check(th_mov_imm(dest, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE));

  /* Conditionally overwrite with 1 on the true path. */
  ot_check(th_it(op, 0x8)); /* IT <cond> (single instruction) */
  ot_check(th_mov_imm(dest, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
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

    /* If result != 0, dest = 1, else dest = 0. Preserve flags from ORRS. */
    ot_check(th_mov_imm(dest, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE));
    ot_check(th_it(0x1, 0x8)); /* IT NE */
    ot_check(th_mov_imm(dest, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
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
    /* Now flags reflect: NE if both non-zero, EQ if either zero.
     * Materialize without clobbering flags before the conditional move.
     */
    ot_check(th_mov_imm(dest, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE));
    ot_check(th_it(0x1, 0x8)); /* IT NE */
    ot_check(th_mov_imm(dest, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }
}

/* Called at end of each IR instruction to clean up scratch register state.
 * - Restores any pushed scratch registers (POP in reverse push order)
 * - Resets global exclusion mask for next instruction */
ST_FUNC void tcc_gen_machine_end_instruction(void)
{
  restore_all_pushed_scratch_regs();
}

ST_FUNC void tcc_gen_machine_vla_op(TACQuadruple *q)
{
  switch (q->op)
  {
  case TCCIR_OP_VLA_ALLOC:
  {
    int align = (int)q->src2.c.i;
    if (align < 8)
      align = 8;
    if (align & (align - 1))
      tcc_error("alignment is not a power of 2: %i", align);

    /* Compute new SP in-place in the size register (the size value is dead after this op). */
    int r = q->src1.pr0;

    /* If src1 wasn't allocated to a register (e.g. constant), load to IP. */
    if (r == PREG_NONE || (r & PREG_SPILLED) || (q->src1.r & VT_VALMASK) == VT_CONST)
    {
      r = R_IP;
      load_to_reg(r, PREG_NONE, &q->src1);
    }

    /* r = SP - r */
    if (r == R_SP)
      tcc_error("compiler_error: VLA alloc picked SP as temp");
    ot_check(th_sub_sp_reg(r, r, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));

    if (align > 1)
    {
      /* Align down: r &= ~(align-1). Prefer immediate encoding. */
      if (!ot(th_bic_imm(r, r, (uint32_t)(align - 1), FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)))
      {
        /* Fallback: materialize mask in a scratch reg and BIC (reg). */
        ScratchRegAlloc mask_alloc = get_scratch_reg_with_save(1u << r);
        int mask_reg = mask_alloc.reg;
        if (!ot(th_generic_mov_imm(mask_reg, align - 1)))
          load_full_const(mask_reg, PREG_NONE, align - 1, NULL);
        ot_check(th_bic_reg(r, r, mask_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        if (mask_alloc.saved)
          ot_check(th_pop(1u << mask_reg));
      }
    }

    ot_check(th_mov_reg(R_SP, r, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
    break;
  }
  case TCCIR_OP_VLA_SP_SAVE:
    /* Save SP to a fixed stack slot (FP-relative). Use IP as scratch. */
    ot_check(th_mov_reg(R_IP, R_SP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
    store(R_IP, &q->dest);
    break;
  case TCCIR_OP_VLA_SP_RESTORE:
    /* Restore SP from a fixed stack slot (FP-relative). Use IP as scratch. */
    load(R_IP, &q->src1);
    ot_check(th_mov_reg(R_SP, R_IP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
    break;
  default:
    tcc_error("compiler_error: tcc_gen_machine_vla_op unsupported op %d", q->op);
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
    /* Float/double to integer conversion.
     * Map based on destination width, not just VT_BTYPE (since VT_LONG is 32-bit on ARM).
     * Use the standard ARM EABI helpers:
     *  - 32-bit: __aeabi_{f,d}2iz / __aeabi_{f,d}2uiz
     *  - 64-bit: __aeabi_{f,d}2lz / __aeabi_{f,d}2ulz
     */
    const int is_float = (src1_size == 4);
    const int is_unsigned = (q->dest.type.t & VT_UNSIGNED) ? 1 : 0;

    if (dest_size == 8)
    {
      return is_unsigned ? (is_float ? "__aeabi_f2ulz" : "__aeabi_d2ulz")
                         : (is_float ? "__aeabi_f2lz" : "__aeabi_d2lz");
    }

    return is_unsigned ? (is_float ? "__aeabi_f2uiz" : "__aeabi_d2uiz") : (is_float ? "__aeabi_f2iz" : "__aeabi_d2iz");
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
