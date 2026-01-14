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

#if defined(TCC_ARM_EABI) && !defined(TCC_ARM_VFP)
#error "Currently TinyCC only supports float computation with VFP instructions"
#endif

#ifndef CONFIG_TCC_CPUVER
#define CONFIG_TCC_CPUVER 5
#endif

#include "arm-thumb-defs.h"
#include "tcc.h"
#include "tccir.h"
#include "tccls.h"
#include "tcctype.h"

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

#define USING_GLOBALS
#include "tcc.h"

/* Target ABI hook: AAPCS-like argument assignment for ARM (R0-R3 + stack).
 *
 * This is a pure layout function: it does not materialize values and does not
 * touch SP. IR can use it to lower calls into explicit CALLSEQ/CALLARG ops.
 */
ST_FUNC int tcc_gen_machine_abi_assign_call_args(const TCCAbiArgDesc *args, int argc, TCCAbiCallLayout *out_layout)
{
  if (!out_layout || (argc > 0 && (!args || !out_layout->locs)))
    return -1;

  int next_reg = 0;  /* Next GP arg register index (0..3) */
  int stack_off = 0; /* Current outgoing stack offset */

  for (int i = 0; i < argc; ++i)
  {
    const TCCAbiArgDesc *ad = &args[i];
    TCCAbiArgLoc *loc = &out_layout->locs[i];

    int size = ad->size;
    int align = ad->alignment;
    if (align < 4)
      align = 4;

    loc->size = (uint16_t)size;
    loc->reg_base = 0;
    loc->reg_count = 0;
    loc->stack_off = 0;

    if (ad->kind == TCC_ABI_ARG_SCALAR64)
    {
      /* 64-bit values require even register alignment and 8-byte stack alignment. */
      if (next_reg & 1)
        next_reg++;
      if (next_reg <= 2)
      {
        loc->kind = TCC_ABI_LOC_REG;
        loc->reg_base = (uint8_t)next_reg;
        loc->reg_count = 2;
        next_reg += 2;
      }
      else
      {
        stack_off = tcc_abi_align_up_int(stack_off, 8);
        loc->kind = TCC_ABI_LOC_STACK;
        loc->stack_off = stack_off;
        stack_off += 8;
        next_reg = 4;
      }
      continue;
    }

    if (ad->kind == TCC_ABI_ARG_STRUCT_BYVAL)
    {
      /* Structs: may occupy multiple 4-byte slots in remaining regs, else stack. */
      int slot_sz = tcc_abi_align_up_int(size, 4);
      int regs_needed = (slot_sz + 3) / 4;
      if (next_reg + regs_needed <= 4)
      {
        loc->kind = TCC_ABI_LOC_REG;
        loc->reg_base = (uint8_t)next_reg;
        loc->reg_count = (uint8_t)regs_needed;
        next_reg += regs_needed;
      }
      else
      {
        stack_off = tcc_abi_align_up_int(stack_off, align);
        loc->kind = TCC_ABI_LOC_STACK;
        loc->stack_off = stack_off;
        stack_off += slot_sz;
        next_reg = 4;
      }
      continue;
    }

    /* Default: 32-bit scalar. */
    if (next_reg <= 3)
    {
      loc->kind = TCC_ABI_LOC_REG;
      loc->reg_base = (uint8_t)next_reg;
      loc->reg_count = 1;
      next_reg++;
    }
    else
    {
      stack_off = tcc_abi_align_up_int(stack_off, 4);
      loc->kind = TCC_ABI_LOC_STACK;
      loc->stack_off = stack_off;
      stack_off += 4;
      next_reg = 4;
    }
  }

  /* AAPCS requires 8-byte SP alignment at call boundary. */
  out_layout->argc = argc;
  out_layout->stack_align = 8;
  out_layout->stack_size = tcc_abi_align_up_int(stack_off, 8);
  return 0;
}

#include "arch/fpu/arm/fpv5-sp-d16.h"
#include "arm-thumb-opcodes.h"

#include <inttypes.h>

static void thumb_backend_dump_svalue(const char *label, const SValue *sv)
{
  if (!sv)
    return;
  fprintf(stderr, "%s: r=0x%x pr0=%d pr1=%d vr=%d c.i=%lld type=0x%x flags=0x%x sym=%p\n", label, sv->r, sv->pr0,
          sv->pr1, sv->vr, (long long)sv->c.i, sv->type.t, sv->type.ref ? sv->type.ref->type.t : 0, (void *)sv->sym);
}

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

/* Register class array - maps each register to its class flags */
ST_DATA const int reg_classes[NB_REGS] = {
    RC_INT | RC_R0,   RC_INT | RC_R1,   RC_INT | RC_R2,   RC_INT | RC_R3,   RC_INT | RC_R12,
    RC_FLOAT | RC_F0, RC_FLOAT | RC_F1, RC_FLOAT | RC_F2, RC_FLOAT | RC_F3,
#ifdef TCC_ARM_VFP
    RC_FLOAT | RC_F4, RC_FLOAT | RC_F5, RC_FLOAT | RC_F6, RC_FLOAT | RC_F7,
#endif
};

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
static void thumb_require_materialized_reg(const char *ctx, const char *operand, int reg);
static void thumb_require_materialized_pair(const char *ctx, const char *operand, int lo, int hi);
static void thumb_ensure_not_spilled(const char *ctx, const char *operand, int reg);
static bool thumb_is_hw_reg(int reg);
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

/* Forward declarations needed by multi-scratch helpers. */
static ScratchRegAlloc get_scratch_reg_with_save(uint32_t exclude_regs);
static void restore_scratch_reg(ScratchRegAlloc *alloc);

typedef struct ScratchRegAllocs
{
  int regs[8];         /* The allocated scratch registers */
  int count;           /* Number of registers allocated */
  uint32_t saved_mask; /* Bitmask of registers that were saved (pushed) */
} ScratchRegAllocs;

static ScratchRegAllocs get_scratch_regs_with_save(uint32_t exclude_regs, int count)
{
  ScratchRegAllocs result;
  memset(&result, 0, sizeof(result));
  if (count <= 0)
    return result;
  if (count > (int)(sizeof(result.regs) / sizeof(result.regs[0])))
    tcc_error("compiler_error: requested too many scratch regs (%d)", count);

  TCCIRState *ir = tcc_state->ir;
  uint32_t exclude = exclude_regs | scratch_global_exclude;
  uint32_t regs_to_save = 0;

#ifdef ARM_THUMB_DEBUG_SCRATCH
  fprintf(stderr, "[SCRATCH] get_scratch_regs: count=%d input_exclude=0x%x global_exclude=0x%x\n", count, exclude_regs,
          scratch_global_exclude);
#endif

  /* First pass: try to find free registers */
  for (int i = 0; i < count; ++i)
  {
    int reg = PREG_NONE;
    if (ir)
    {
      reg = tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, exclude, ir->leaffunc);
    }

    if (reg != PREG_NONE && reg >= 0 && reg < 16)
    {
      /* Found a free register */
#ifdef ARM_THUMB_DEBUG_SCRATCH
      fprintf(stderr, "[SCRATCH] -> reg[%d]=%d (free)\n", i, reg);
#endif
      result.regs[i] = reg;
      exclude |= (1u << reg);
      /* R11 and R12 are permanent scratch registers and can be reused freely.
       * Don't add them to global exclude. */
      if (reg != 11 && reg != 12)
        scratch_global_exclude |= (1u << reg);
      result.count++;
    }
    else
    {
      /* Need to save a register - select one based on priority */
      int reg_to_save = -1;
      if (!(exclude & (1 << R_IP)))
      {
        reg_to_save = R_IP;
      }
      else if (ir && ir->leaffunc && !(exclude & (1 << R_LR)))
      {
        reg_to_save = R_LR;
      }
      else
      {
        /* Try R0-R3 */
        for (int r = 0; r <= 3; ++r)
        {
          if (!(exclude & (1 << r)))
          {
            reg_to_save = r;
            break;
          }
        }
      }

      if (reg_to_save < 0)
      {
        /* Try R4-R10, skipping R11/R12 which are reserved for call argument processing */
        for (int r = 4; r <= 10; ++r)
        {
          if (!(exclude & (1 << r)))
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

#ifdef ARM_THUMB_DEBUG_SCRATCH
      fprintf(stderr, "[SCRATCH] -> reg[%d]=%d (will save)\n", i, reg_to_save);
#endif
      result.regs[i] = reg_to_save;
      regs_to_save |= (1u << reg_to_save);
      exclude |= (1u << reg_to_save);
      result.count++;
    }
  }

  /* Second pass: emit a single PUSH for all registers that need saving */
  if (regs_to_save != 0)
  {
#ifdef ARM_THUMB_DEBUG_SCRATCH
    fprintf(stderr, "[SCRATCH] Pushing registers (mask=0x%x) in single instruction\n", regs_to_save);
#endif
    ot_check(th_push(regs_to_save));
    result.saved_mask = regs_to_save;

    /* Track each saved register in the push stack for proper LIFO ordering */
    for (int i = 0; i < count; ++i)
    {
      if (regs_to_save & (1u << result.regs[i]))
      {
        if (scratch_push_count < 128)
        {
          scratch_push_stack[scratch_push_count++] = result.regs[i];
        }
        else
        {
          tcc_error("compiler_error: scratch register push stack overflow (>128 pushes without restore)");
        }
      }
    }
  }

  return result;
}

static void restore_scratch_regs(ScratchRegAllocs *allocs)
{
  if (!allocs || allocs->count <= 0)
    return;

  if (allocs->saved_mask != 0)
  {
    /* Check if we can restore all saved registers in LIFO order */
    int can_restore_all = 1;
    int check_count = 0;

    /* Count how many saved registers we have and verify LIFO order */
    for (int i = allocs->count - 1; i >= 0 && can_restore_all; --i)
    {
      if (allocs->saved_mask & (1u << allocs->regs[i]))
      {
        int stack_idx = scratch_push_count - 1 - check_count;
        if (stack_idx < 0 || scratch_push_stack[stack_idx] != allocs->regs[i])
        {
          can_restore_all = 0;
        }
        check_count++;
      }
    }

    if (can_restore_all && check_count > 0)
    {
      /* We can restore all saved registers with a single POP */
#ifdef ARM_THUMB_DEBUG_SCRATCH
      fprintf(stderr, "[SCRATCH] Popping registers (mask=0x%x) in single instruction\n", allocs->saved_mask);
#endif
      ot_check(th_pop(allocs->saved_mask));

      /* Update the push stack and global exclude */
      scratch_push_count -= check_count;
      for (int i = 0; i < allocs->count; ++i)
      {
        if (allocs->saved_mask & (1u << allocs->regs[i]))
        {
          scratch_global_exclude &= ~(1u << allocs->regs[i]);
        }
      }
      allocs->saved_mask = 0;
    }
    else
    {
      /* Cannot restore in order - defer to individual restore or end-of-instruction cleanup */
#ifdef ARM_THUMB_DEBUG_SCRATCH
      fprintf(stderr, "[SCRATCH] WARNING: restore_scratch_regs out of order; deferring POP\n");
#endif
      /* Keep saved_mask set so cleanup knows these need restoration */
    }
  }

  allocs->count = 0;
}
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

#ifdef ARM_THUMB_DEBUG_SCRATCH
  fprintf(stderr, "[SCRATCH] get_scratch_reg: input_exclude=0x%x global_exclude=0x%x\n", exclude_regs,
          scratch_global_exclude);
#endif

  exclude_regs |= scratch_global_exclude;

  if (ir)
  {
    int reg = tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, exclude_regs, ir->leaffunc);
    /* tcc_ls_find_free_scratch_reg() returns PREG_NONE (0xFF) if none.
     * Do not treat that as a valid register (it would encode as PC and fault).
     */
    if (reg != PREG_NONE && reg >= 0 && reg < 16)
    {
#ifdef ARM_THUMB_DEBUG_SCRATCH
      fprintf(stderr, "[SCRATCH] -> returning reg=%d (free) exclude=0x%x\n", reg, exclude_regs);
#endif
      result.reg = reg;
      result.saved = 0;
      /* Update global exclude so subsequent calls won't return the same register.
       * Exception: R11/R12 are reserved for call argument processing and can be
       * reused freely without exclusion. They must never be saved/pushed. */
      if (reg != 11 && reg != 12)
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
#ifdef ARM_THUMB_DEBUG_SCRATCH
  fprintf(stderr, "[SCRATCH] WARNING: no free scratch register! Saving r%d to stack\n", reg_to_save);
#endif
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
    /* We MUST restore in strict LIFO order.
     * An out-of-order POP corrupts SP (and can crash under QEMU).
     * If callers restore out of order, defer the POP to end-of-instruction
     * cleanup (restore_all_pushed_scratch_regs), and keep the register
     * excluded so it cannot be reused before it is actually restored.
     */
    if (scratch_push_count > 0 && scratch_push_stack[scratch_push_count - 1] == alloc->reg)
    {
      ot_check(th_pop(1 << alloc->reg));
      alloc->saved = 0;
      scratch_push_count--;
      scratch_global_exclude &= ~(1u << alloc->reg);
    }
    else
    {
      if (scratch_push_count > 0)
      {
#ifdef ARM_THUMB_DEBUG_SCRATCH
        fprintf(stderr,
                "[SCRATCH] WARNING: restore_scratch_reg out of order; deferring POP "
                "reg=%d (top=%d)\n",
                alloc->reg, scratch_push_stack[scratch_push_count - 1]);
#endif
      }
      else
      {
#ifdef ARM_THUMB_DEBUG_SCRATCH
        fprintf(stderr, "[SCRATCH] WARNING: restore_scratch_reg with empty push stack; deferring POP reg=%d\n",
                alloc->reg);
#endif
      }
      return;
    }
  }

  /* Always release from global exclude for non-saved scratch regs. */
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
#ifdef ARM_THUMB_DEBUG_SCRATCH
    fprintf(stderr, "[SCRATCH] auto-restoring r%d (push order %d)\n", reg, i);
#endif
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

  if (flags & TCC_MACHINE_SCRATCH_AVOID_CALL_ARG_REGS)
  {
    exclude_regs |= (1u << R0) | (1u << R1) | (1u << R2) | (1u << R3);
  }

  if (flags & TCC_MACHINE_SCRATCH_AVOID_PERM_SCRATCH)
  {
    exclude_regs |= (1u << R11) | (1u << R12);
  }

  ScratchRegAlloc first = get_scratch_reg_with_save(exclude_regs);
  if (first.reg == PREG_NONE)
    tcc_error("compiler_error: unable to allocate scratch register");

  scratch->regs[0] = first.reg;
  scratch->reg_count = 1;
  if (first.saved)
    scratch->saved_mask |= 1u;
  exclude_regs |= (1u << first.reg);
  /* Update global exclude so subsequent scratch allocations don't get same register.
   * Exception: R11 and R12 are permanent scratch registers and can be reused. */
  if (first.reg != 11 && first.reg != 12)
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
    /* Update global exclude for pair's second register too.
     * Exception: R11 and R12 are permanent scratch registers and can be reused. */
    if (second.reg != 11 && second.reg != 12)
      scratch_global_exclude |= (1u << second.reg);
  }
}

ST_FUNC void tcc_machine_release_scratch(const TCCMachineScratchRegs *scratch)
{
  if (!scratch)
    return;

  /* IMPORTANT: scratch registers are acquired via get_scratch_reg_with_save(),
   * which records PUSH order in scratch_push_stack for end-of-instruction cleanup.
   * Releasing must therefore go through restore_scratch_reg() so the push-stack
   * accounting stays consistent (otherwise restore_all_pushed_scratch_regs() may
   * POP registers a second time and corrupt the stack).
   */
  for (int i = scratch->reg_count - 1; i >= 0; --i)
  {
    ScratchRegAlloc alloc = {0};
    alloc.reg = scratch->regs[i];
    alloc.saved = (scratch->saved_mask & (1u << i)) != 0;
    restore_scratch_reg(&alloc);
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
  // if (!is_valid_opcode(op))
  // {
  //   tcc_error("compiler_error: received invalid opcode: 0x%x\n", op.opcode);
  // }
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
  /* R12 (IP) is the standard inter-procedure scratch register.
   * R11 is also available for allocation but reserved during call argument processing. */
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
  thumb_gen_state.call_sites_by_id = NULL;
  thumb_gen_state.call_sites_by_id_size = 0;
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
  thumb_free_call_sites();
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
  if (val == 0)
    return;

  if (val > 0)
  {
    thumb_opcode add_imm = th_add_sp_imm(R_SP, (uint32_t)val, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
    if (is_valid_opcode(add_imm))
    {
      ot(add_imm);
      return;
    }

    /* Large adjustment: materialize value into IP and add via register form. */
    load_full_const(R_IP, PREG_NONE, (int64_t)val, NULL);
    ot_check(th_add_sp_reg(R_SP, R_IP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE, THUMB_SHIFT_DEFAULT));
    return;
  }

  /* val < 0 */
  const uint32_t sub = (uint32_t)(-val);
  thumb_opcode sub_imm = th_sub_sp_imm(R_SP, sub, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  if (is_valid_opcode(sub_imm))
  {
    ot(sub_imm);
    return;
  }

  load_full_const(R_IP, PREG_NONE, (int64_t)sub, NULL);
  ot_check(th_sub_sp_reg(R_SP, R_IP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
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
  {
    SValue target = *vtop;
    tcc_ir_put(tcc_state->ir, TCCIR_OP_IJUMP, &target, NULL, NULL);
  }
  vtop--;
  print_vstack("ggoto");
}

ST_FUNC void tcc_gen_machine_indirect_jump_op(TACQuadruple *q)
{
  /* Indirect jump: target address in src1 register.
   * If VT_LVAL is set, src1.pr0 holds a pointer to the target address,
   * and we need to load the actual target address before jumping. */
  if (q->src1.pr0 == PREG_NONE)
  {
    tcc_error("internal error: IJUMP target not in a register");
  }

  int target_reg = q->src1.pr0;
  ScratchRegAlloc scratch = {0};

  /* Check if we need to dereference: VT_LVAL means the register holds a pointer
   * to the target address, not the target address itself */
  const int val_kind = q->src1.r & VT_VALMASK;
  const int is_address_of = (val_kind == VT_LOCAL || val_kind == VT_LLOCAL) && !(q->src1.r & VT_LVAL);
  const int needs_deref = (q->src1.r & VT_LVAL) && !is_address_of;

  if (needs_deref)
  {
    /* Load the target address from memory pointed to by src1.pr0 */
    /* We can reuse the same register if it's not special, otherwise get a scratch */
    if (target_reg < 8)
    {
      /* Load target address: target_reg = *target_reg (word load, offset 0) */
      ot_check(th_ldr_imm(target_reg, target_reg, 0, 6, ENFORCE_ENCODING_NONE));
    }
    else
    {
      /* High register - need scratch for the load */
      scratch = get_scratch_reg_with_save(0);
      ot_check(th_ldr_imm(scratch.reg, target_reg, 0, 6, ENFORCE_ENCODING_NONE));
      target_reg = scratch.reg;
    }
  }

  ot_check(th_bx_reg((uint16_t)target_reg));

  if (scratch.saved)
  {
    ot_check(th_pop(1u << scratch.reg));
  }
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

ST_FUNC int tcc_machine_can_encode_stack_offset_for_reg(int frame_offset, int dest_reg)
{
  /* Check if frame_offset can be directly encoded in ldr/str instructions
   * without requiring a scratch register. This is used to avoid wasteful
   * address materialization when the backend can handle the offset directly.
   * Tests with dest_reg since encoding availability depends on the register. */
  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
  const int sign = (frame_offset < 0);
  const int abs_offset = sign ? -frame_offset : frame_offset;

  /* Try to encode as ldr instruction with the actual destination register.
   * Some encodings (e.g., Thumb-1 T1) only work with low registers (r0-r7). */
  const thumb_opcode ins = th_ldr_imm(dest_reg, base_reg, abs_offset, sign ? 4 : 6, ENFORCE_ENCODING_NONE);
  return (ins.size != 0);
}

ST_FUNC int tcc_machine_can_encode_stack_offset_with_param_adj(int frame_offset, int is_param, int dest_reg)
{
  /* Like tcc_machine_can_encode_stack_offset_for_reg, but applies offset_to_args for VT_PARAM.
   * Stack parameters need offset_to_args adjustment (prologue push size). */
  int offset = frame_offset;
  if (is_param)
    offset += offset_to_args;
  return tcc_machine_can_encode_stack_offset_for_reg(offset, dest_reg);
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

static void th_store32_imm_or_reg(int src_reg, uint32_t base_reg, int abs_off, int sign)
{
  if (!ot(th_str_imm(src_reg, base_reg, abs_off, sign ? 4 : 6, ENFORCE_ENCODING_NONE)))
  {
    ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(abs_off, sign, (1u << src_reg) | (1u << base_reg));
    int rr = rr_alloc.reg;
    ot_check(th_str_reg(src_reg, base_reg, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&rr_alloc);
  }
}

static void th_store16_imm_or_reg(int src_reg, uint32_t base_reg, int abs_off, int sign)
{
  if (!ot(th_strh_imm(src_reg, base_reg, abs_off, sign ? 4 : 6, ENFORCE_ENCODING_NONE)))
  {
    ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(abs_off, sign, (1u << src_reg) | (1u << base_reg));
    int rr = rr_alloc.reg;
    ot_check(th_strh_reg(src_reg, base_reg, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&rr_alloc);
  }
}

static void th_store8_imm_or_reg(int src_reg, uint32_t base_reg, int abs_off, int sign)
{
  if (!ot(th_strb_imm(src_reg, base_reg, abs_off, sign ? 4 : 6, ENFORCE_ENCODING_NONE)))
  {
    ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(abs_off, sign, (1u << src_reg) | (1u << base_reg));
    int rr = rr_alloc.reg;
    ot_check(th_strb_reg(src_reg, base_reg, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&rr_alloc);
  }
}

static uint32_t th_store_resolve_base(int src_reg, SValue *sv, int ft, int *abs_off, int *sign,
                                      ScratchRegAlloc *base_alloc, int *has_base_alloc)
{
  int off = sv->c.i;
  if (off >= 0)
    *sign = 0;
  else
  {
    *sign = 1;
    off = -off;
  }
  *abs_off = off;
  *has_base_alloc = 0;

  uint32_t base_reg = R_FP;
  int fr = sv->r;
  int v = fr & VT_VALMASK;

  if ((fr & VT_LVAL) && v < VT_CONST)
  {
    /* Lvalue address already in a register. Prefer materialized address in sv->pr0
     * (IR paths) but fall back to legacy encoding in sv->r.
     */
    base_reg = (sv->pr0 != PREG_NONE) ? sv->pr0 : v;
    thumb_require_materialized_reg("store", "address base", base_reg);
    *abs_off = 0;
    *sign = 0;
    return base_reg;
  }

  if ((fr & VT_LVAL) && v == VT_CONST)
  {
    /* Global symbol lvalue: load the base address (without offset) into a scratch reg.
     * Keep the scratch reg live until the actual store is emitted.
     */
    SValue v1;
    Sym *validated_sym = (sv->r & VT_SYM) ? validate_sym_for_reloc(sv->sym) : NULL;
    memset(&v1, 0, sizeof(SValue));
    v1.type.t = ft;
    v1.r = (fr & ~VT_LVAL) | (validated_sym ? VT_SYM : 0);
    v1.c.i = 0;
    v1.sym = validated_sym;
    v1.pr0 = PREG_NONE;

    uint32_t exclude_regs = (1u << src_reg);
    *base_alloc = get_scratch_reg_with_save(exclude_regs);
    base_reg = base_alloc->reg;
    *has_base_alloc = 1;

    load(base_reg, &v1);
    return base_reg;
  }

  /* Default: stack/local address (FP-based). */
  return base_reg;
}

void store(int r, SValue *sv)
{
  int v, ft, fr;
  TRACE("'store' reg: %d", r);

  /* IR owns spills: backend store must never be asked to store from a spilled
   * sentinel or a non-hardware register.
   *
   * For hard-float, `r` may be a VFP register (TREG_F0..TREG_F7). Otherwise it
   * must be an integer HW register.
   */
  if (r == PREG_NONE || (r & PREG_SPILLED))
    tcc_error("compiler_error: store called with non-materialized source reg %d", r);
  if (tcc_state->float_abi == ARM_HARD_FLOAT && r >= TREG_F0 && r <= TREG_F7)
  {
    /* ok: VFP source */
  }
  else
  {
    /* Must be an integer hardware register. */
    thumb_require_materialized_reg("store", "src", r);
  }

  fr = sv->r;
  ft = sv->type.t;
  v = fr & VT_VALMASK;

  /* Handle register-to-register store (destination is a physical register, not memory).
   * This happens when storing to a parameter that lives in a callee-saved register. */
  if (!(fr & VT_LVAL) && fr != VT_LOCAL && sv->pr0 != PREG_NONE && thumb_is_hw_reg(sv->pr0))
  {
    int dest_reg = sv->pr0;
    thumb_require_materialized_reg("store", "dest", dest_reg);
    if (dest_reg != r)
    {
      ot_check(
          th_mov_reg(dest_reg, r, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
    }
    /* For 64-bit types, also move the high word */
    int src_btype = ft & VT_BTYPE;
    if ((src_btype == VT_DOUBLE || src_btype == VT_LDOUBLE || src_btype == VT_LLONG) && sv->pr1 != PREG_NONE)
    {
      /* The caller should have set up pr1 for the destination high register.
       * The source high register comes from sv->pr1 passed by the caller. */
      /* Actually, for store() the source high is implicit (r+1 or caller-provided).
       * For now, just handle the case where sv->pr1 is the dest high. */
    }
    return;
  }

  if (fr & VT_LVAL || fr == VT_LOCAL)
  {
    int abs_off, sign;
    ScratchRegAlloc base_alloc = (ScratchRegAlloc){0};
    int has_base_alloc = 0;
    uint32_t base = th_store_resolve_base(r, sv, ft, &abs_off, &sign, &base_alloc, &has_base_alloc);

    /* Check if source is VFP or integer register.
     * Only use VFP instructions if hard float ABI is enabled.
     */
    if (is_float(ft))
    {
      if (tcc_state->float_abi == ARM_HARD_FLOAT && r >= TREG_F0 && r <= TREG_F7)
      {
        /* VFP source - use VSTR */
        if ((ft & VT_BTYPE) != VT_FLOAT)
          ot_check(th_vstr(base, r, !sign, 1, abs_off));
        else
          ot_check(th_vstr(base, r, !sign, 0, abs_off));
      }
      else
      {
        /* Soft-float (or integer-reg float values): use integer stores. */
        if ((ft & VT_BTYPE) == VT_FLOAT)
        {
          th_store32_imm_or_reg(r, base, abs_off, sign);
        }
        else
        {
          /* Double precision - two 32-bit stores (low word first).
           * IR owns spills: the caller must provide an explicit high-word
           * register in sv->pr1; do not guess r+1.
           */
          int r_high = sv->pr1;
          if (r_high == PREG_NONE)
          {
            /* Legacy (non-IR) backend paths may still call store() with only
             * the low register. In that case, assume a conventional register
             * pair (low=r, high=r+1). */
            if (thumb_is_hw_reg(r) && thumb_is_hw_reg(r + 1) && (r + 1) != R_SP && (r + 1) != R_PC)
              r_high = r + 1;
            else
              tcc_error("compiler_error: cannot store double - missing source high register (sv->pr1)");
          }
          thumb_require_materialized_reg("store", "src.high", r_high);
          if (r_high == R_SP || r_high == R_PC)
            tcc_error("compiler_error: cannot store double - invalid source high register %d", r_high);

          /* High word is at +4 from low word. When sign=1 (negative offset),
           * we need to decrease abs_off to get a higher address. */
          int hi_abs_off = sign ? (abs_off - 4) : (abs_off + 4);
          th_store32_imm_or_reg(r, base, abs_off, sign);
          th_store32_imm_or_reg(r_high, base, hi_abs_off, sign);
        }
      }
    }
    else if ((ft & VT_BTYPE) == VT_SHORT)
    {
      th_store16_imm_or_reg(r, base, abs_off, sign);
    }
    else if ((ft & VT_BTYPE) == VT_BYTE)
    {
      th_store8_imm_or_reg(r, base, abs_off, sign);
    }
    else if ((ft & VT_BTYPE) == VT_LLONG)
    {
      /* Long long - store both low and high words */
      int r_high = sv->pr1;
      if (r_high == PREG_NONE)
      {
        /* Legacy (non-IR) backend paths may still call store() with only the
         * low register. Assume the value is in a register pair (r, r+1). */
        if (thumb_is_hw_reg(r) && thumb_is_hw_reg(r + 1) && (r + 1) != R_SP && (r + 1) != R_PC)
          r_high = r + 1;
        else
          tcc_error("compiler_error: cannot store llong - missing source high register (sv->pr1)");
      }
      thumb_require_materialized_reg("store", "src.high", r_high);
      if (r_high == R_SP || r_high == R_PC)
        tcc_error("compiler_error: cannot store llong - invalid source high register %d", r_high);

      /* High word is at +4 from low word. When sign=1 (negative offset),
       * we need to decrease abs_off to get a higher address. */
      int hi_abs_off = sign ? (abs_off - 4) : (abs_off + 4);
      th_store32_imm_or_reg(r, base, abs_off, sign);
      th_store32_imm_or_reg(r_high, base, hi_abs_off, sign);
    }
    else
    {
      TRACE("store: sign: %x, r: %x, base: %x, off: %x", sign, r, base, abs_off);
      th_store32_imm_or_reg(r, base, abs_off, sign);
      TRACE("done");
    }

    if (has_base_alloc)
      restore_scratch_reg(&base_alloc);
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

ST_FUNC void tcc_machine_addr_of_stack_slot(int dest_reg, int frame_offset, int is_param)
{
  if (dest_reg == PREG_NONE)
    tcc_error("compiler_error: addr_of_stack_slot requires a destination register");

  /* Stack parameters live above the saved-register area.
   * When computing their address, fold in offset_to_args (prologue push size). */
  if (is_param)
    frame_offset += offset_to_args;

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

/* Load a constant value into a register (or register pair for 64-bit).
 * This is a simplified wrapper around load_full_const/th_generic_mov_imm
 * that doesn't require an SValue. Used by IR-level materialization.
 * If sym is non-NULL, a relocation will be generated for symbol-relative constants. */
ST_FUNC void tcc_machine_load_constant(int dest_reg, int dest_reg_high, int64_t value, int is_64bit, Sym *sym)
{
  if (dest_reg == PREG_NONE)
    tcc_error("compiler_error: load_constant requires a destination register");

  /* Symbol-relative constants always need the literal pool for relocations */
  if (sym)
  {
    Sym *validated_sym = validate_sym_for_reloc(sym);
    if (validated_sym)
    {
      load_full_const(dest_reg, dest_reg_high, value, validated_sym);
      return;
    }
    /* Invalid or missing sym - fall through to treat as plain constant */
  }

  if (is_64bit)
  {
    if (dest_reg_high == PREG_NONE)
      tcc_error("compiler_error: 64-bit load_constant requires high register");

    const uint32_t lo = (uint32_t)(value & 0xFFFFFFFF);
    const uint32_t hi = (uint32_t)((uint64_t)value >> 32);

    /* Try immediate encoding for both halves */
    thumb_opcode o1 = th_generic_mov_imm(dest_reg, (int)lo);
    thumb_opcode o2 = th_generic_mov_imm(dest_reg_high, (int)hi);

    if (o1.size != 0 && o2.size != 0)
    {
      /* Both can be encoded as immediates */
      ot(o1);
      ot(o2);
      return;
    }

    /* At least one half needs literal pool - use combined 64-bit load */
    load_full_const(dest_reg, dest_reg_high, value, NULL);
    return;
  }

  /* 32-bit constant */
  if (!ot(th_generic_mov_imm(dest_reg, (uint32_t)value)))
    load_full_const(dest_reg, PREG_NONE, value, NULL);
}

/* Load comparison result (0 or 1) based on condition flags.
 * Used by IR-level materialization for VT_CMP values. */
ST_FUNC void tcc_machine_load_cmp_result(int dest_reg, int condition_code)
{
  if (dest_reg == PREG_NONE)
    tcc_error("compiler_error: load_cmp_result requires a destination register");
  if (dest_reg == R_SP || dest_reg == R_PC)
    tcc_error("compiler_error: load_cmp_result cannot use SP or PC");

  const uint32_t firstcond = mapcc(condition_code);
  /* IT block: if cond then mov 1, else mov 0 */
  o(0xbf00 | (firstcond << 4) | 0x4 | ((~firstcond & 1) << 3));
  ot_check(th_generic_mov_imm(dest_reg, 1));
  ot_check(th_generic_mov_imm(dest_reg, 0));
}

/* Load jump condition result (0 or 1) based on a pending jump target.
 * Used by IR-level materialization for VT_JMP/VT_JMPI values. */
ST_FUNC void tcc_machine_load_jmp_result(int dest_reg, int jmp_addr, int invert)
{
  if (dest_reg == PREG_NONE)
    tcc_error("compiler_error: load_jmp_result requires a destination register");

#ifdef TCC_TARGET_ARM_ARCHV6M
  if (dest_reg > 7)
    tcc_error("compiler_error: implement load_jmp_result for armv6m with high register");
#endif

  /* Load the "true" branch value, then unconditionally branch over the "false" value,
   * then patch the jump target to land on the "false" value */
  ot_check(th_generic_mov_imm(dest_reg, invert ? 0 : 1));
  ot_check(th_b_t4(2));
  gsym(jmp_addr);
  ot_check(th_generic_mov_imm(dest_reg, invert ? 1 : 0));
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
     * integer path. Only use VFP if hard float ABI is enabled. */
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

void load_vt_local(int r, SValue *sv, int base)
{
  int off = sv->c.i;
  /* Stack parameters live above the saved-register area.
   * When computing their address, fold in offset_to_args (prologue push size).
   */
  if (sv->r & VT_PARAM)
  {
    off += offset_to_args;
  }

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

void load_to_dest(SValue *dest, SValue *sv)
{
  int v, ft, fr, sign;
  int64_t fc;
  fr = sv->r;
  ft = sv->type.t;
  fc = sv->c.i;
  int btype = ft & VT_BTYPE;
  const char *ctx = "load_to_dest";

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

    if (v == VT_LLOCAL)
    {
      /* VT_LLOCAL is a direct stack lvalue at FP/SP + offset.
       * Do NOT treat it as an extra level of indirection (pointer stored on stack).
       * The old behavior caused double-dereferences like:
       *   ldr r0, [fp, off]; ldr rX, [r0]
       * which breaks plain locals (e.g. loop indices) and struct-init tests.
       */
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
      /* Address-in-register lvalue. Prefer sv->pr0 when it carries a real register
       * number, otherwise fall back to the legacy encoding in sv->r (v). */
      if (sv->pr0 != PREG_NONE)
      {
        thumb_require_materialized_reg(ctx, "lvalue base", sv->pr0);
        base = sv->pr0;
      }
      else
      {
        base = v;
      }
      fc = 0;
      sign = 0;
      v = VT_LOCAL;
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
  {
    /* Route through machine API for constants */
    Sym *sym = (sv->r & VT_SYM) ? sv->sym : NULL;
    int is_64bit = tcc_is_64bit_operand(sv);
    return tcc_machine_load_constant(dest->pr0, dest->pr1, sv->c.i, is_64bit, sym);
  }
  else if (v == VT_LOCAL)
  {
    /* Address-of stack slot/local. Spills are materialized in IR codegen. */
    int base = R_FP;
    if (tcc_state->need_frame_pointer == 0)
    {
      base = R_SP;
    }
    return load_vt_local(dest->pr0, sv, base);
  }
  else if (v == VT_CMP)
    return tcc_machine_load_cmp_result(dest->pr0, sv->c.i);
  else if (v == VT_JMP || v == VT_JMPI)
    return tcc_machine_load_jmp_result(dest->pr0, sv->c.i, v == VT_JMPI);
  else if (v < VT_CONST)
  {
    /* For IR-generated code, use pr0 as the source register */
    int src_reg = v;
    if (sv->pr0 != PREG_NONE)
    {
      thumb_require_materialized_reg(ctx, "source register", sv->pr0);
      src_reg = sv->pr0;
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

typedef thumb_opcode (*thumb_imm_handler_t)(uint32_t rd, uint32_t rn, uint32_t imm,
                                            thumb_flags_behaviour flags_behaviour,
                                            thumb_enforce_encoding enforce_encoding);
typedef thumb_opcode (*thumb_reg_handler_t)(uint32_t rd, uint32_t rn, uint32_t rm,
                                            thumb_flags_behaviour flags_behaviour, thumb_shift shift_type,
                                            thumb_enforce_encoding enforce_encoding);
typedef struct ThumbDataProcessingHandler
{
  thumb_imm_handler_t imm_handler;
  thumb_reg_handler_t reg_handler;
} ThumbDataProcessingHandler;

static void thumb_require_materialized_reg(const char *ctx, const char *operand, int reg)
{
  const bool reg_is_hw = (reg >= 0) && (reg <= 15);
  if (reg == PREG_NONE || (reg & PREG_SPILLED) || !reg_is_hw)
  {
    tcc_error("compiler_error: %s expects %s in a physical register (pr=%d)", ctx, operand, reg);
  }
}

static void thumb_require_materialized_pair(const char *ctx, const char *operand, int lo, int hi)
{
  thumb_require_materialized_reg(ctx, operand, lo);
  thumb_require_materialized_reg(ctx, operand, hi);
}

static void thumb_ensure_not_spilled(const char *ctx, const char *operand, int reg)
{
  if (reg != PREG_NONE)
  {
    const bool reg_is_hw = (reg >= 0) && (reg <= 15);
    if ((reg & PREG_SPILLED) || !reg_is_hw)
    {
      tcc_error("compiler_error: %s operand %s unexpectedly spilled", ctx, operand);
    }
  }
}

static uint32_t thumb_exclude_mask_for_regs(int count, const int *regs)
{
  uint32_t mask = 0;
  for (int i = 0; i < count; ++i)
  {
    const int reg = regs[i];
    if (reg >= 0 && reg <= 15)
      mask |= (1u << reg);
  }
  return mask;
}

static bool thumb_is_hw_reg(int reg)
{
  return reg >= 0 && reg <= 15;
}

static void thumb_prepare_dest_pair_for_64bit_op(const char *ctx, SValue *dest, int *rd_low, int *rd_high,
                                                 ScratchRegAlloc *rd_low_alloc, ScratchRegAlloc *rd_high_alloc,
                                                 bool *store_low, bool *store_high, uint32_t *exclude_mask)
{
  if (!dest || !rd_low || !rd_high || !rd_low_alloc || !rd_high_alloc || !store_low || !store_high || !exclude_mask)
    tcc_error("compiler_error: invalid arguments to thumb_prepare_dest_pair_for_64bit_op");

  *rd_low = dest->pr0;
  *rd_high = dest->pr1;
  *store_low = false;
  *store_high = false;

  /* If the chosen destination register overlaps with an excluded register
   * (typically a live source operand), do not write the result in-place.
   * Materialize into scratch and store back afterward.
   *
   * This matters for ops like UMULL and 64-bit shifts where the machine
   * instruction sequence expects sources to remain intact while producing
   * a 64-bit result.
   */
  if (thumb_is_hw_reg(*rd_low) && ((*exclude_mask & (1u << *rd_low)) == 0))
  {
    thumb_require_materialized_reg(ctx, "dest.low", *rd_low);
    *exclude_mask |= (1u << *rd_low);
  }
  else
  {
    *rd_low_alloc = get_scratch_reg_with_save(*exclude_mask);
    *rd_low = rd_low_alloc->reg;
    *store_low = true;
    *exclude_mask |= (1u << *rd_low);
  }

  if (thumb_is_hw_reg(*rd_high) && ((*exclude_mask & (1u << *rd_high)) == 0))
  {
    thumb_require_materialized_reg(ctx, "dest.high", *rd_high);
    *exclude_mask |= (1u << *rd_high);
  }
  else
  {
    *rd_high_alloc = get_scratch_reg_with_save(*exclude_mask);
    *rd_high = rd_high_alloc->reg;
    *store_high = true;
    *exclude_mask |= (1u << *rd_high);
  }
}

static void thumb_store_dest_pair_if_needed(SValue *dest, int rd_low, int rd_high, bool store_low, bool store_high)
{
  if (!dest)
    return;

  if (store_low)
    store(rd_low, dest);
  if (store_high)
  {
    SValue dest_hi = *dest;
    dest_hi.c.i += 4;
    store(rd_high, &dest_hi);
  }
}

static void thumb_emit_add_imm_fallback(int rd, int rn, uint32_t imm, thumb_flags_behaviour flags)
{
  thumb_opcode add_low = th_add_imm(rd, rn, imm, flags, ENFORCE_ENCODING_NONE);
  if (add_low.size == 0)
  {
    uint32_t exclude = 0;
    if (rd >= 0 && rd <= 15)
      exclude |= (1u << rd);
    if (rn >= 0 && rn <= 15)
      exclude |= (1u << rn);
    ScratchRegAlloc scratch = get_scratch_reg_with_save(exclude);
    tcc_machine_load_constant(scratch.reg, PREG_NONE, (int32_t)imm, 0, NULL);
    ot_check(th_add_reg(rd, rn, scratch.reg, flags, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&scratch);
  }
  else
  {
    ot_check(add_low);
  }
}

static void thumb_emit_sub_imm_fallback(int rd, int rn, uint32_t imm, thumb_flags_behaviour flags)
{
  thumb_opcode sub_low = th_sub_imm(rd, rn, imm, flags, ENFORCE_ENCODING_NONE);
  if (sub_low.size == 0)
  {
    uint32_t exclude = 0;
    if (rd >= 0 && rd <= 15)
      exclude |= (1u << rd);
    if (rn >= 0 && rn <= 15)
      exclude |= (1u << rn);
    ScratchRegAlloc scratch = get_scratch_reg_with_save(exclude);
    tcc_machine_load_constant(scratch.reg, PREG_NONE, (int32_t)imm, 0, NULL);
    ot_check(th_sub_reg(rd, rn, scratch.reg, flags, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&scratch);
  }
  else
  {
    ot_check(sub_low);
  }
}

static void thumb_emit_op_imm_fallback(int rd, int rn, uint32_t imm, thumb_flags_behaviour flags,
                                       ThumbDataProcessingHandler handler)
{
  thumb_opcode sub_low = handler.imm_handler(rd, rn, imm, flags, ENFORCE_ENCODING_NONE);
  if (sub_low.size == 0)
  {
    uint32_t exclude = 0;
    if (rd >= 0 && rd <= 15)
      exclude |= (1u << rd);
    if (rn >= 0 && rn <= 15)
      exclude |= (1u << rn);
    ScratchRegAlloc scratch = get_scratch_reg_with_save(exclude);
    tcc_machine_load_constant(scratch.reg, PREG_NONE, (int32_t)imm, 0, NULL);
    ot_check(handler.reg_handler(rd, rn, scratch.reg, flags, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&scratch);
  }
  else
  {
    ot_check(sub_low);
  }
}

static void thumb_emit_opcode64_imm(TACQuadruple *op, const char *ctx, ThumbDataProcessingHandler regular,
                                    ThumbDataProcessingHandler carry)
{
  const bool src2_is_imm = th_has_immediate_value(op->src2.r) || op->src2.pr0 == PREG_NONE;
  const uint64_t src2_imm = (uint64_t)op->src2.c.i;
  const uint32_t imm_low = (uint32_t)(src2_imm & 0xffffffffu);
  const uint32_t imm_high = (uint32_t)(src2_imm >> 32);

  /* dest might not be in physical regs (e.g. lives in memory). */
  uint32_t exclude = 0;
  ScratchRegAlloc rd_low_alloc = {0};
  ScratchRegAlloc rd_high_alloc = {0};
  bool store_low = false;
  bool store_high = false;
  int rd_low = op->dest.pr0;
  int rd_high = op->dest.pr1;
  thumb_prepare_dest_pair_for_64bit_op(ctx, &op->dest, &rd_low, &rd_high, &rd_low_alloc, &rd_high_alloc, &store_low,
                                       &store_high, &exclude);

  const bool src1_is64 = is_64bit_type(op->src1.type.t);
  const bool src2_is64 = is_64bit_type(op->src2.type.t);

  /* Materialize src1. */
  const bool src1_is_imm = (op->src1.pr0 == PREG_NONE) && th_has_immediate_value(op->src1.r);
  int rn_low = op->src1.pr0;
  int rn_high = (src1_is64 ? op->src1.pr1 : PREG_NONE);
  ScratchRegAlloc rn_low_alloc = {0};
  ScratchRegAlloc rn_high_alloc = {0};

  if (src1_is_imm)
  {
    Sym *sym = (op->src1.r & VT_SYM) ? op->src1.sym : NULL;
    if (src1_is64)
    {
      tcc_machine_load_constant(rd_low, rd_high, op->src1.c.i, 1, sym);
      rn_low = rd_low;
      rn_high = rd_high;
    }
    else
    {
      tcc_machine_load_constant(rd_low, PREG_NONE, op->src1.c.i, 0, sym);
      rn_low = rd_low;
      rn_high = PREG_NONE;
    }
  }
  else if (thumb_is_hw_reg(rn_low) && (!src1_is64 || (rn_high != PREG_NONE && thumb_is_hw_reg(rn_high))))
  {
    thumb_require_materialized_reg(ctx, "src1.low", rn_low);
    if (src1_is64 && rn_high != PREG_NONE)
      thumb_ensure_not_spilled(ctx, "src1.high", rn_high);
    exclude |= (1u << rn_low);
    if (src1_is64 && rn_high != PREG_NONE)
      exclude |= (1u << rn_high);
  }
  else
  {
    rn_low_alloc = get_scratch_reg_with_save(exclude);
    rn_low = rn_low_alloc.reg;
    exclude |= (1u << rn_low);
    if (src1_is64)
    {
      rn_high_alloc = get_scratch_reg_with_save(exclude);
      rn_high = rn_high_alloc.reg;
      exclude |= (1u << rn_high);
      load_to_reg(rn_low, rn_high, &op->src1);
    }
    else
    {
      rn_high = PREG_NONE;
      load_to_reg(rn_low, PREG_NONE, &op->src1);
    }
  }

  /* Materialize src2 (if not immediate). */
  int rm_low = op->src2.pr0;
  int rm_high = (src2_is64 ? op->src2.pr1 : PREG_NONE);
  ScratchRegAlloc rm_low_alloc = {0};
  ScratchRegAlloc rm_high_alloc = {0};
  if (!src2_is_imm)
  {
    if (thumb_is_hw_reg(rm_low) && (!src2_is64 || (rm_high != PREG_NONE && thumb_is_hw_reg(rm_high))))
    {
      thumb_require_materialized_reg(ctx, "src2.low", rm_low);
      if (src2_is64 && rm_high != PREG_NONE)
        thumb_ensure_not_spilled(ctx, "src2.high", rm_high);
    }
    else
    {
      rm_low_alloc = get_scratch_reg_with_save(exclude);
      rm_low = rm_low_alloc.reg;
      exclude |= (1u << rm_low);
      if (src2_is64)
      {
        rm_high_alloc = get_scratch_reg_with_save(exclude);
        rm_high = rm_high_alloc.reg;
        exclude |= (1u << rm_high);
        load_to_reg(rm_low, rm_high, &op->src2);
      }
      else
      {
        rm_high = PREG_NONE;
        load_to_reg(rm_low, PREG_NONE, &op->src2);
      }
    }
  }
  else
  {
    rm_low = PREG_NONE;
    rm_high = PREG_NONE;
  }

  /* Low word sets carry/flags for the high word. */
  if (src2_is_imm)
    thumb_emit_op_imm_fallback(rd_low, rn_low, imm_low, FLAGS_BEHAVIOUR_SET, regular);
  else
    ot_check(
        regular.reg_handler(rd_low, rn_low, rm_low, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));

  if (src2_is_imm)
  {
    if (rn_high != PREG_NONE)
    {
      ot_check(carry.imm_handler(rd_high, rn_high, imm_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    }
    else
    {
      ot_check(th_mov_imm(rd_high, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      ot_check(carry.imm_handler(rd_high, rd_high, imm_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    }
  }
  else if (rn_high != PREG_NONE && rm_high != PREG_NONE)
  {
    ot_check(carry.reg_handler(rd_high, rn_high, rm_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                               ENFORCE_ENCODING_NONE));
  }
  else if (rn_high != PREG_NONE)
  {
    ot_check(carry.imm_handler(rd_high, rn_high, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }
  else if (rm_high != PREG_NONE)
  {
    ot_check(th_mov_imm(rd_high, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    ot_check(carry.reg_handler(rd_high, rd_high, rm_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                               ENFORCE_ENCODING_NONE));
  }
  else
  {
    ot_check(th_mov_imm(rd_high, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    ot_check(carry.imm_handler(rd_high, rd_high, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }

  thumb_store_dest_pair_if_needed(&op->dest, rd_low, rd_high, store_low, store_high);
  restore_scratch_reg(&rm_high_alloc);
  restore_scratch_reg(&rm_low_alloc);
  restore_scratch_reg(&rn_high_alloc);
  restore_scratch_reg(&rn_low_alloc);
  restore_scratch_reg(&rd_high_alloc);
  restore_scratch_reg(&rd_low_alloc);
}

typedef uint64_t (*thumb_u64_fold_t)(uint64_t lhs, uint64_t rhs);
typedef uint32_t (*thumb_u32_fold_t)(uint32_t lhs, uint32_t rhs);

static uint64_t thumb_fold_u64_or(uint64_t lhs, uint64_t rhs)
{
  return lhs | rhs;
}
static uint64_t thumb_fold_u64_and(uint64_t lhs, uint64_t rhs)
{
  return lhs & rhs;
}
static uint64_t thumb_fold_u64_xor(uint64_t lhs, uint64_t rhs)
{
  return lhs ^ rhs;
}
static uint32_t thumb_fold_u32_or(uint32_t lhs, uint32_t rhs)
{
  return lhs | rhs;
}
static uint32_t thumb_fold_u32_and(uint32_t lhs, uint32_t rhs)
{
  return lhs & rhs;
}
static uint32_t thumb_fold_u32_xor(uint32_t lhs, uint32_t rhs)
{
  return lhs ^ rhs;
}

static void thumb_materialize_u32(int rd, uint32_t value)
{
  SValue imm_sv;
  memset(&imm_sv, 0, sizeof(imm_sv));
  imm_sv.r = VT_CONST;
  imm_sv.type.t = VT_INT | VT_UNSIGNED;
  imm_sv.c.i = value;
  load_to_reg(rd, PREG_NONE, &imm_sv);
}

static void thumb_emit_dp_imm_with_fallback(ThumbDataProcessingHandler handler, int rd, int rn, uint32_t imm,
                                            uint32_t exclude_mask)
{
  thumb_opcode op = handler.imm_handler(rd, rn, imm, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
  if (op.size == 0)
  {
    if (thumb_is_hw_reg(rd))
      exclude_mask |= (1u << rd);
    if (thumb_is_hw_reg(rn))
      exclude_mask |= (1u << rn);
    ScratchRegAlloc scratch = get_scratch_reg_with_save(exclude_mask);
    thumb_materialize_u32(scratch.reg, imm);
    ot_check(handler.reg_handler(rd, rn, scratch.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                 ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&scratch);
  }
  else
  {
    ot_check(op);
  }
}

static void thumb_emit_logical64_op(TACQuadruple *op, ThumbDataProcessingHandler handler, thumb_u64_fold_t fold64,
                                    thumb_u32_fold_t fold32, const char *ctx)
{
  const bool src1_is_imm = th_has_immediate_value(op->src1.r) || op->src1.pr0 == PREG_NONE;
  const bool src2_is_imm = th_has_immediate_value(op->src2.r) || op->src2.pr0 == PREG_NONE;
  const uint64_t src1_imm = (uint64_t)op->src1.c.i;
  const uint64_t src2_imm = (uint64_t)op->src2.c.i;

  if (src1_is_imm && src2_is_imm)
  {
    /* Constant folding: load the computed result directly to destination */
    int64_t folded_value = (int64_t)fold64(src1_imm, src2_imm);
    int is_64bit = tcc_is_64bit_operand(&op->dest);
    tcc_machine_load_constant(op->dest.pr0, op->dest.pr1, folded_value, is_64bit, NULL);
    return;
  }

  ScratchRegAlloc rd_low_alloc = {0};
  ScratchRegAlloc rd_high_alloc = {0};
  bool store_low = false;
  bool store_high = false;
  int rd_low = op->dest.pr0;
  int rd_high = op->dest.pr1;
  uint32_t dest_exclude = 0;

  if (src1_is_imm || src2_is_imm)
  {
    const SValue *reg_src = src1_is_imm ? &op->src2 : &op->src1;
    const uint64_t imm64 = src1_is_imm ? src1_imm : src2_imm;
    const uint32_t imm_low = (uint32_t)(imm64 & 0xffffffffu);
    const uint32_t imm_high = (uint32_t)(imm64 >> 32);
    const bool reg_src_is64 = is_64bit_type(reg_src->type.t);

    thumb_require_materialized_reg(ctx, "src.low", reg_src->pr0);
    if (reg_src_is64 && reg_src->pr1 != PREG_NONE)
      thumb_require_materialized_reg(ctx, "src.high", reg_src->pr1);
    else
      thumb_ensure_not_spilled(ctx, "src.high", reg_src->pr1);

    const int rn_low = reg_src->pr0;
    const int rn_high = (reg_src_is64 && reg_src->pr1 != PREG_NONE) ? reg_src->pr1 : PREG_NONE;
    const int mask_regs_for_dest[] = {rn_low, rn_high};
    dest_exclude = thumb_exclude_mask_for_regs(2, mask_regs_for_dest);
    thumb_prepare_dest_pair_for_64bit_op(ctx, &op->dest, &rd_low, &rd_high, &rd_low_alloc, &rd_high_alloc, &store_low,
                                         &store_high, &dest_exclude);

    uint32_t imm_exclude = 0;
    if (thumb_is_hw_reg(rd_low))
      imm_exclude |= (1u << rd_low);
    if (thumb_is_hw_reg(rd_high))
      imm_exclude |= (1u << rd_high);
    if (thumb_is_hw_reg(rn_low))
      imm_exclude |= (1u << rn_low);
    if (thumb_is_hw_reg(rn_high))
      imm_exclude |= (1u << rn_high);

    thumb_emit_dp_imm_with_fallback(handler, rd_low, rn_low, imm_low, imm_exclude);

    if (rn_high == PREG_NONE)
    {
      const uint32_t folded_high = fold32(0u, imm_high);
      thumb_materialize_u32(rd_high, folded_high);
    }
    else
    {
      thumb_emit_dp_imm_with_fallback(handler, rd_high, rn_high, imm_high, imm_exclude);
    }

    goto thumb_logical64_cleanup;
  }

  const bool src1_is64 = is_64bit_type(op->src1.type.t);
  const bool src2_is64 = is_64bit_type(op->src2.type.t);

  /* Check if sources are spilled and need reload */
  const bool src1_lo_spilled = (op->src1.pr0 != PREG_NONE) && (op->src1.pr0 & PREG_SPILLED);
  const bool src1_hi_spilled = (op->src1.pr1 != PREG_NONE) && (op->src1.pr1 & PREG_SPILLED);
  const bool src2_lo_spilled = (op->src2.pr0 != PREG_NONE) && (op->src2.pr0 & PREG_SPILLED);
  const bool src2_hi_spilled = (op->src2.pr1 != PREG_NONE) && (op->src2.pr1 & PREG_SPILLED);

  int src1_lo = op->src1.pr0;
  int src1_hi = op->src1.pr1;
  int src2_lo = op->src2.pr0;
  int src2_hi = op->src2.pr1;
  ScratchRegAlloc src1_lo_alloc = {0};
  ScratchRegAlloc src1_hi_alloc = {0};
  ScratchRegAlloc src2_lo_alloc = {0};
  ScratchRegAlloc src2_hi_alloc = {0};
  uint32_t src_exclude = 0;

  /* Reload spilled src1.low */
  if (src1_lo_spilled)
  {
    src1_lo_alloc = get_scratch_reg_with_save(src_exclude);
    src1_lo = src1_lo_alloc.reg;
    if (thumb_is_hw_reg(src1_lo))
      src_exclude |= (1u << src1_lo);
    load_to_reg(src1_lo, PREG_NONE, &op->src1);
  }
  else
  {
    thumb_require_materialized_reg(ctx, "src1.low", src1_lo);
    if (thumb_is_hw_reg(src1_lo))
      src_exclude |= (1u << src1_lo);
  }

  /* Reload spilled src1.high */
  if (src1_hi_spilled)
  {
    src1_hi_alloc = get_scratch_reg_with_save(src_exclude);
    src1_hi = src1_hi_alloc.reg;
    if (thumb_is_hw_reg(src1_hi))
      src_exclude |= (1u << src1_hi);
    SValue src1_hi_val = op->src1;
    src1_hi_val.c.i += 4;
    load_to_reg(src1_hi, PREG_NONE, &src1_hi_val);
  }
  else if (src1_is64 && src1_hi != PREG_NONE)
  {
    thumb_require_materialized_reg(ctx, "src1.high", src1_hi);
    if (thumb_is_hw_reg(src1_hi))
      src_exclude |= (1u << src1_hi);
  }
  else
  {
    thumb_ensure_not_spilled(ctx, "src1.high", src1_hi);
  }

  /* Reload spilled src2.low */
  if (src2_lo_spilled)
  {
    src2_lo_alloc = get_scratch_reg_with_save(src_exclude);
    src2_lo = src2_lo_alloc.reg;
    if (thumb_is_hw_reg(src2_lo))
      src_exclude |= (1u << src2_lo);
    load_to_reg(src2_lo, PREG_NONE, &op->src2);
  }
  else
  {
    thumb_require_materialized_reg(ctx, "src2.low", src2_lo);
    if (thumb_is_hw_reg(src2_lo))
      src_exclude |= (1u << src2_lo);
  }

  /* Reload spilled src2.high */
  if (src2_hi_spilled)
  {
    src2_hi_alloc = get_scratch_reg_with_save(src_exclude);
    src2_hi = src2_hi_alloc.reg;
    if (thumb_is_hw_reg(src2_hi))
      src_exclude |= (1u << src2_hi);
    SValue src2_hi_val = op->src2;
    src2_hi_val.c.i += 4;
    load_to_reg(src2_hi, PREG_NONE, &src2_hi_val);
  }
  else if (src2_is64 && src2_hi != PREG_NONE)
  {
    thumb_require_materialized_reg(ctx, "src2.high", src2_hi);
    if (thumb_is_hw_reg(src2_hi))
      src_exclude |= (1u << src2_hi);
  }
  else
  {
    thumb_ensure_not_spilled(ctx, "src2.high", src2_hi);
  }

  const int src1_high = (src1_is64 && src1_hi != PREG_NONE) ? src1_hi : PREG_NONE;
  const int src2_high = (src2_is64 && src2_hi != PREG_NONE) ? src2_hi : PREG_NONE;
  const int mask_regs_for_dest[] = {src1_lo, src1_high, src2_lo, src2_high};
  dest_exclude = thumb_exclude_mask_for_regs(4, mask_regs_for_dest);
  thumb_prepare_dest_pair_for_64bit_op(ctx, &op->dest, &rd_low, &rd_high, &rd_low_alloc, &rd_high_alloc, &store_low,
                                       &store_high, &dest_exclude);

  ot_check(handler.reg_handler(rd_low, src1_lo, src2_lo, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                               ENFORCE_ENCODING_NONE));

  const bool src1_high_valid = thumb_is_hw_reg(src1_high);
  const bool src2_high_valid = thumb_is_hw_reg(src2_high);
  if (!src1_high_valid && !src2_high_valid)
  {
    thumb_materialize_u32(rd_high, fold32(0u, 0u));
  }
  else if (!src1_high_valid || !src2_high_valid)
  {
    const int available = src1_high_valid ? src1_high : src2_high;
    uint32_t exclude = 0;
    if (thumb_is_hw_reg(rd_low))
      exclude |= (1u << rd_low);
    if (thumb_is_hw_reg(rd_high))
      exclude |= (1u << rd_high);
    if (thumb_is_hw_reg(src1_lo))
      exclude |= (1u << src1_lo);
    if (thumb_is_hw_reg(src2_lo))
      exclude |= (1u << src2_lo);
    if (thumb_is_hw_reg(available))
      exclude |= (1u << available);
    thumb_emit_dp_imm_with_fallback(handler, rd_high, available, 0u, exclude);
  }
  else
  {
    ot_check(handler.reg_handler(rd_high, src1_high, src2_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                 ENFORCE_ENCODING_NONE));
  }

thumb_logical64_cleanup:
  thumb_store_dest_pair_if_needed(&op->dest, rd_low, rd_high, store_low, store_high);
  restore_scratch_reg(&rd_high_alloc);
  restore_scratch_reg(&rd_low_alloc);
  restore_scratch_reg(&src2_hi_alloc);
  restore_scratch_reg(&src2_lo_alloc);
  restore_scratch_reg(&src1_hi_alloc);
  restore_scratch_reg(&src1_lo_alloc);
}

static void thumb_emit_shift64_imm(TACQuadruple *op, const char *ctx, bool is_left, thumb_imm_handler_t dst_lo_shift,
                                   thumb_imm_handler_t dst_hi_shift, thumb_imm_handler_t cross_shift,
                                   bool sign_extend_missing_hi, bool arith_right)
{
  const uint32_t sh = (uint32_t)op->src2.c.i;

  int dst_lo = op->dest.pr0;
  int dst_hi = op->dest.pr1;
  ScratchRegAlloc dst_lo_alloc = (ScratchRegAlloc){0};
  ScratchRegAlloc dst_hi_alloc = (ScratchRegAlloc){0};
  bool store_lo = false;
  bool store_hi = false;
  uint32_t exclude = 0;

  /* For shifts, dest might not be assigned a physical register (e.g. value lives in memory).
     Use scratch regs in that case, then store the result back. */
  thumb_prepare_dest_pair_for_64bit_op(ctx, &op->dest, &dst_lo, &dst_hi, &dst_lo_alloc, &dst_hi_alloc, &store_lo,
                                       &store_hi, &exclude);

  int src_lo = op->src1.pr0;
  int src_hi = op->src1.pr1;
  ScratchRegAlloc src_lo_alloc = (ScratchRegAlloc){0};
  ScratchRegAlloc src_hi_alloc = (ScratchRegAlloc){0};

  const bool src_is_imm = (src_lo == PREG_NONE) && th_has_immediate_value(op->src1.r);
  if (src_is_imm)
  {
    Sym *sym = (op->src1.r & VT_SYM) ? op->src1.sym : NULL;
    tcc_machine_load_constant(dst_lo, dst_hi, op->src1.c.i, 1, sym);
    src_lo = dst_lo;
    src_hi = dst_hi;
  }
  else
  {
    const bool src_lo_spilled = (src_lo != PREG_NONE) && (src_lo & PREG_SPILLED);
    const bool src_hi_spilled = (src_hi != PREG_NONE) && (src_hi & PREG_SPILLED);

    /* Low word must be usable as a register input. */
    if (src_lo == PREG_NONE || src_lo_spilled || (op->src1.r & VT_LVAL) || th_has_immediate_value(op->src1.r))
    {
      src_lo_alloc = get_scratch_reg_with_save(exclude);
      src_lo = src_lo_alloc.reg;
      if (thumb_is_hw_reg(src_lo))
        exclude |= (1u << src_lo);
      load_to_reg(src_lo, PREG_NONE, &op->src1);
    }
    else
    {
      thumb_require_materialized_reg(ctx, "src1.low", src_lo);
      if (thumb_is_hw_reg(src_lo))
        exclude |= (1u << src_lo);
    }

    /* High word may be missing (treated as 0 or sign-extension), but if it exists it must be usable too. */
    if (src_hi != PREG_NONE && !src_hi_spilled)
    {
      thumb_require_materialized_reg(ctx, "src1.high", src_hi);
      if (thumb_is_hw_reg(src_hi))
        exclude |= (1u << src_hi);
    }
    else if (src_hi_spilled || (op->src1.r & VT_LVAL))
    {
      /* Lvalue source: load high word from (addr + 4) into a scratch. */
      src_hi_alloc = get_scratch_reg_with_save(exclude);
      src_hi = src_hi_alloc.reg;
      if (thumb_is_hw_reg(src_hi))
        exclude |= (1u << src_hi);
      SValue src1_hi = op->src1;
      src1_hi.c.i += 4;
      load_to_reg(src_hi, PREG_NONE, &src1_hi);
    }
    else
    {
      thumb_ensure_not_spilled(ctx, "src1.high", src_hi);
    }
  }

  if (src_hi == PREG_NONE)
  {
    if (sign_extend_missing_hi)
    {
      /* Sign-extend missing high word from src_lo. */
      ot_check(th_asr_imm(dst_hi, src_lo, 31, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    }
    else
    {
      ot_check(th_mov_imm(dst_hi, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    }
    src_hi = dst_hi;
  }

  if (sh == 0)
  {
    ot_check(
        th_mov_reg(dst_lo, src_lo, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
    ot_check(
        th_mov_reg(dst_hi, src_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
    goto thumb_shift64_cleanup;
  }

  if (sh < 32)
  {
    const int regs_for_mask[] = {dst_lo, dst_hi, src_lo, src_hi};
    ScratchRegAlloc tmp_alloc = get_scratch_reg_with_save(thumb_exclude_mask_for_regs(4, regs_for_mask) | exclude);

    if (is_left)
    {
      /* dst_lo = src_lo << sh */
      ot_check(dst_lo_shift(dst_lo, src_lo, sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      /* tmp = src_lo >> (32 - sh) */
      ot_check(cross_shift(tmp_alloc.reg, src_lo, 32 - sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      /* dst_hi = (src_hi << sh) | tmp */
      ot_check(dst_hi_shift(dst_hi, src_hi, sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      ot_check(th_orr_reg(dst_hi, dst_hi, tmp_alloc.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
    }
    else
    {
      /* tmp = src_hi << (32 - sh) */
      ot_check(cross_shift(tmp_alloc.reg, src_hi, 32 - sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      /* dst_lo = (src_lo >> sh) | tmp (low word always logical right shift) */
      ot_check(th_lsr_imm(dst_lo, src_lo, sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      ot_check(th_orr_reg(dst_lo, dst_lo, tmp_alloc.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      /* dst_hi = src_hi >> sh (logical or arithmetic depending on op) */
      ot_check(dst_hi_shift(dst_hi, src_hi, sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    }

    restore_scratch_reg(&tmp_alloc);
    goto thumb_shift64_cleanup;
  }

  if (sh == 32)
  {
    if (is_left)
    {
      ot_check(th_mov_imm(dst_lo, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      ot_check(
          th_mov_reg(dst_hi, src_lo, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
    }
    else
    {
      ot_check(
          th_mov_reg(dst_lo, src_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
      if (arith_right)
        ot_check(th_asr_imm(dst_hi, src_hi, 31, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      else
        ot_check(th_mov_imm(dst_hi, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    }
    goto thumb_shift64_cleanup;
  }

  if (sh < 64)
  {
    if (is_left)
    {
      ot_check(th_mov_imm(dst_lo, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      ot_check(dst_hi_shift(dst_hi, src_lo, sh - 32, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    }
    else
    {
      /* dst_lo = src_hi >> (sh - 32) (logical for SHR, arithmetic for SAR) */
      ot_check(dst_hi_shift(dst_lo, src_hi, sh - 32, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      if (arith_right)
        ot_check(th_asr_imm(dst_hi, src_hi, 31, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      else
        ot_check(th_mov_imm(dst_hi, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    }
    goto thumb_shift64_cleanup;
  }

  /* sh >= 64 */
  if (is_left)
  {
    ot_check(th_mov_imm(dst_lo, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    ot_check(th_mov_imm(dst_hi, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }
  else if (arith_right)
  {
    ot_check(th_asr_imm(dst_hi, src_hi, 31, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    ot_check(
        th_mov_reg(dst_lo, dst_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
  }
  else
  {
    ot_check(th_mov_imm(dst_lo, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    ot_check(th_mov_imm(dst_hi, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }

thumb_shift64_cleanup:
  thumb_store_dest_pair_if_needed(&op->dest, dst_lo, dst_hi, store_lo, store_hi);
  restore_scratch_reg(&src_hi_alloc);
  restore_scratch_reg(&src_lo_alloc);
  restore_scratch_reg(&dst_hi_alloc);
  restore_scratch_reg(&dst_lo_alloc);
}

typedef thumb_opcode (*thumb_regonly3_handler_t)(uint32_t rd, uint32_t rn, uint32_t rm);

static thumb_opcode thumb_mul_regonly(uint32_t rd, uint32_t rn, uint32_t rm)
{
  return th_mul(rd, rn, rm, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
}

static thumb_opcode thumb_sdiv_regonly(uint32_t rd, uint32_t rn, uint32_t rm)
{
  return th_sdiv((uint16_t)rd, (uint16_t)rn, (uint16_t)rm);
}

static thumb_opcode thumb_udiv_regonly(uint32_t rd, uint32_t rn, uint32_t rm)
{
  return th_udiv((uint16_t)rd, (uint16_t)rn, (uint16_t)rm);
}

/* NOTE: thumb_materialize_binop32_sources() has been removed.
 * Constant-to-register materialization is now handled by IR-level
 * tcc_ir_materialize_const_to_reg() in tccir.c. Backend functions like
 * thumb_emit_regonly_binop32() now only handle VT_LVAL fallback. */

static void thumb_emit_regonly_binop32(TACQuadruple *op, thumb_regonly3_handler_t emitter, const char *ctx)
{
  int rd = op->dest.pr0;
  if (rd == PREG_NONE)
    tcc_error("compiler_error: %s missing destination register", ctx);
  thumb_require_materialized_reg(ctx, "dest", rd);

  /* IR-level tcc_ir_materialize_const_to_reg() now handles constant-to-register
   * conversion for register-only operations. Operands should already be in registers. */
  int rn = op->src1.pr0;
  int rm = op->src2.pr0;

  /* Fall back to backend materialization for VT_LVAL (memory loads) that
   * weren't handled by IR-level materialization */
  ScratchRegAlloc rn_alloc = {0};
  ScratchRegAlloc rm_alloc = {0};
  uint32_t exclude = (1u << rd);

  if (rn == PREG_NONE || (op->src1.r & VT_LVAL))
  {
    rn_alloc = get_scratch_reg_with_save(exclude);
    rn = rn_alloc.reg;
    exclude |= (1u << rn);
    load_to_reg(rn, PREG_NONE, &op->src1);
  }
  else
  {
    thumb_require_materialized_reg(ctx, "src1", rn);
  }

  if (rm == PREG_NONE || (op->src2.r & VT_LVAL))
  {
    rm_alloc = get_scratch_reg_with_save(exclude);
    rm = rm_alloc.reg;
    load_to_reg(rm, PREG_NONE, &op->src2);
  }
  else
  {
    thumb_require_materialized_reg(ctx, "src2", rm);
  }

  ot_check(emitter((uint32_t)rd, (uint32_t)rn, (uint32_t)rm));
  restore_scratch_reg(&rm_alloc);
  restore_scratch_reg(&rn_alloc);
}

static void thumb_emit_mod32(TACQuadruple *op, thumb_regonly3_handler_t div_emitter, const char *ctx)
{
  int dest_reg = op->dest.pr0;
  if (dest_reg == PREG_NONE)
    tcc_error("compiler_error: %s missing destination register", ctx);
  thumb_require_materialized_reg(ctx, "dest", dest_reg);

  /* IR-level tcc_ir_materialize_const_to_reg() now handles constant-to-register
   * conversion for register-only operations. Operands should already be in registers. */
  int src1_reg = op->src1.pr0;
  int src2_reg = op->src2.pr0;

  /* Fall back to backend materialization for VT_LVAL (memory loads) */
  ScratchRegAlloc src1_alloc = {0};
  ScratchRegAlloc src2_alloc = {0};
  ScratchRegAlloc quotient_alloc = {0};
  uint32_t exclude_regs = (1u << dest_reg);

  if (src1_reg == PREG_NONE || (op->src1.r & VT_LVAL))
  {
    src1_alloc = get_scratch_reg_with_save(exclude_regs);
    src1_reg = src1_alloc.reg;
    exclude_regs |= (1u << src1_reg);
    load_to_reg(src1_reg, PREG_NONE, &op->src1);
  }
  else
  {
    thumb_require_materialized_reg(ctx, "src1", src1_reg);
    exclude_regs |= (1u << src1_reg);
  }

  if (src2_reg == PREG_NONE || (op->src2.r & VT_LVAL))
  {
    src2_alloc = get_scratch_reg_with_save(exclude_regs);
    src2_reg = src2_alloc.reg;
    exclude_regs |= (1u << src2_reg);
    load_to_reg(src2_reg, PREG_NONE, &op->src2);
  }
  else
  {
    thumb_require_materialized_reg(ctx, "src2", src2_reg);
    exclude_regs |= (1u << src2_reg);
  }

  /* quotient = src1 / src2 */
  quotient_alloc = get_scratch_reg_with_save(exclude_regs);
  const int quotient = quotient_alloc.reg;
  ot_check(div_emitter((uint32_t)quotient, (uint32_t)src1_reg, (uint32_t)src2_reg));
  /* quotient *= src2 */
  ot_check(thumb_mul_regonly((uint32_t)quotient, (uint32_t)quotient, (uint32_t)src2_reg));
  /* dest = src1 - quotient */
  ot_check(th_sub_reg(dest_reg, src1_reg, quotient, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                      ENFORCE_ENCODING_NONE));

  restore_scratch_reg(&quotient_alloc);
  restore_scratch_reg(&src2_alloc);
  restore_scratch_reg(&src1_alloc);
}

static void thumb_emit_mul32(TACQuadruple *op)
{
  thumb_emit_regonly_binop32(op, thumb_mul_regonly, "MUL");
}

typedef thumb_opcode (*thumb_longmul_handler_t)(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm);

static void thumb_emit_longmul32x32_to64(TACQuadruple *op, thumb_longmul_handler_t emitter, const char *ctx)
{
  int rn = op->src1.pr0;
  int rm = op->src2.pr0;
  ScratchRegAlloc rn_alloc = {0};
  ScratchRegAlloc rm_alloc = {0};

  uint32_t exclude = 0;

  if (rn == PREG_NONE || (op->src1.r & VT_LVAL) || th_has_immediate_value(op->src1.r))
  {
    rn_alloc = get_scratch_reg_with_save(exclude);
    rn = rn_alloc.reg;
    exclude |= (1u << rn);
    load_to_reg(rn, PREG_NONE, &op->src1);
  }
  else
  {
    thumb_require_materialized_reg(ctx, "src1", rn);
    if (thumb_is_hw_reg(rn))
      exclude |= (1u << rn);
  }

  if (rm == PREG_NONE || (op->src2.r & VT_LVAL) || th_has_immediate_value(op->src2.r))
  {
    rm_alloc = get_scratch_reg_with_save(exclude);
    rm = rm_alloc.reg;
    exclude |= (1u << rm);
    load_to_reg(rm, PREG_NONE, &op->src2);
  }
  else
  {
    thumb_require_materialized_reg(ctx, "src2", rm);
    if (thumb_is_hw_reg(rm))
      exclude |= (1u << rm);
  }

  ScratchRegAlloc rd_low_alloc = {0};
  ScratchRegAlloc rd_high_alloc = {0};
  bool store_low = false;
  bool store_high = false;
  int rd_low = op->dest.pr0;
  int rd_high = op->dest.pr1;

  thumb_prepare_dest_pair_for_64bit_op(ctx, &op->dest, &rd_low, &rd_high, &rd_low_alloc, &rd_high_alloc, &store_low,
                                       &store_high, &exclude);

  ot_check(emitter(rd_low, rd_high, rn, rm));

  thumb_store_dest_pair_if_needed(&op->dest, rd_low, rd_high, store_low, store_high);
  restore_scratch_reg(&rd_high_alloc);
  restore_scratch_reg(&rd_low_alloc);
  restore_scratch_reg(&rm_alloc);
  restore_scratch_reg(&rn_alloc);
}

static void thumb_process_data64_op(TACQuadruple *op)
{
  ThumbDataProcessingHandler regular_handler;
  ThumbDataProcessingHandler carry_handler;
  const char *context = "unk";
  switch (op->op)
  {
  case TCCIR_OP_UMULL:
  {
    thumb_emit_longmul32x32_to64(op, th_umull, "UMULL");
    return;
  }
  case TCCIR_OP_ADD:
  {
    regular_handler.imm_handler = th_add_imm;
    regular_handler.reg_handler = th_add_reg;
    carry_handler.imm_handler = th_adc_imm;
    carry_handler.reg_handler = th_adc_reg;
    context = "64-bit ADD";
  }
  break;
  case TCCIR_OP_SUB:
  {
    regular_handler.imm_handler = th_sub_imm;
    regular_handler.reg_handler = th_sub_reg;
    carry_handler.imm_handler = th_sbc_imm;
    carry_handler.reg_handler = th_sbc_reg;
    context = "64-bit SUB";
  }
  break;
  case TCCIR_OP_SHL:
  {
    if (!th_has_immediate_value(op->src2.r))
      tcc_error("compiler_error: 64-bit SHL expects immediate shift count");
    thumb_emit_shift64_imm(op, "64-bit SHL", true, th_lsl_imm, th_lsl_imm, th_lsr_imm, false, false);
    return;
  }
  case TCCIR_OP_SHR:
  {
    if (!th_has_immediate_value(op->src2.r))
      tcc_error("compiler_error: 64-bit SHR expects immediate shift count");
    thumb_emit_shift64_imm(op, "64-bit SHR", false, th_lsr_imm, th_lsr_imm, th_lsl_imm, false, false);
    return;
  }
  case TCCIR_OP_SAR:
  {
    if (!th_has_immediate_value(op->src2.r))
      tcc_error("compiler_error: 64-bit SAR expects immediate shift count");
    thumb_emit_shift64_imm(op, "64-bit SAR", false, th_lsr_imm, th_asr_imm, th_lsl_imm, true, true);
    return;
  }
  case TCCIR_OP_OR:
  {
    ThumbDataProcessingHandler logical;
    logical.imm_handler = th_orr_imm;
    logical.reg_handler = th_orr_reg;
    return thumb_emit_logical64_op(op, logical, thumb_fold_u64_or, thumb_fold_u32_or, "64-bit OR");
  }
  case TCCIR_OP_AND:
  {
    ThumbDataProcessingHandler logical;
    logical.imm_handler = th_and_imm;
    logical.reg_handler = th_and_reg;
    return thumb_emit_logical64_op(op, logical, thumb_fold_u64_and, thumb_fold_u32_and, "64-bit AND");
  }
  break;
  case TCCIR_OP_XOR:
  {
    ThumbDataProcessingHandler logical;
    logical.imm_handler = th_eor_imm;
    logical.reg_handler = th_eor_reg;
    return thumb_emit_logical64_op(op, logical, thumb_fold_u64_xor, thumb_fold_u32_xor, "64-bit XOR");
  }
  break;
  default:
    tcc_error("compiler_error: unsupported 64-bit data processing operation: %d", op->op);
    break;
  }

  return thumb_emit_opcode64_imm(op, context, regular_handler, carry_handler);
}

static void thumb_emit_data_processing_op32(TACQuadruple *op, ThumbDataProcessingHandler handler,
                                            thumb_flags_behaviour flags)
{
  const char *ctx = tcc_ir_get_op_name(op->op);

  int src1_reg = op->src1.pr0;
  int src2_reg = op->src2.pr0;

  const bool src1_is_imm = th_has_immediate_value(op->src1.r);
  const bool src2_is_imm = th_has_immediate_value(op->src2.r);

  const bool src1_is_address_of = ((op->src1.r & VT_VALMASK) == VT_LOCAL) && !(op->src1.r & VT_LVAL);
  const bool src2_is_address_of = ((op->src2.r & VT_VALMASK) == VT_LOCAL) && !(op->src2.r & VT_LVAL);

  /* VT_LVAL on a non-local operand means "register holds pointer, dereference it" */
  const bool src1_is_lval = (op->src1.r & VT_LVAL) && !src1_is_address_of;
  const bool src2_is_lval = (op->src2.r & VT_LVAL) && !src2_is_address_of;

  const bool src1_needs_load = src1_is_imm || src1_is_address_of || src1_is_lval || src1_reg == PREG_NONE;
  const bool src2_needs_load = src2_is_imm || src2_is_address_of || src2_is_lval || src2_reg == PREG_NONE;

  uint32_t exclude_regs = 0;
  ScratchRegAlloc src1_alloc = {0};
  ScratchRegAlloc src2_alloc = {0};

  const bool dest_sets_flags = (op->op == TCCIR_OP_CMP);
  int dest_reg = op->dest.pr0;
  if (dest_reg == PREG_NONE)
  {
    if (!dest_sets_flags)
    {
      tcc_error("compiler_error: %s missing destination register after materialization", ctx);
    }
    /* CMP only sets flags; the encoding ignores Rd. Use R0 to keep encoders happy. */
    dest_reg = R0;
  }
  else
  {
    thumb_require_materialized_reg(ctx, "dest", dest_reg);
    if (thumb_is_hw_reg(dest_reg))
      exclude_regs |= (1u << dest_reg);
  }

  /* If src2 is already in a register, exclude it too so src1 doesn't clobber it */
  if (!src2_is_imm && !src2_is_address_of && thumb_is_hw_reg(src2_reg))
  {
    exclude_regs |= (1u << src2_reg);
  }

  if (src1_needs_load)
  {
    src1_alloc = get_scratch_reg_with_save(exclude_regs);
    src1_reg = src1_alloc.reg;
    if (thumb_is_hw_reg(src1_reg))
      exclude_regs |= (1u << src1_reg);
    load_to_reg(src1_reg, PREG_NONE, &op->src1);
  }
  else
  {
    thumb_require_materialized_reg(ctx, "src1", src1_reg);
    if (thumb_is_hw_reg(src1_reg))
      exclude_regs |= (1u << src1_reg);
  }

  if (src2_is_imm)
  {
    /* Try immediate form first; if it doesn't encode, fall back to loading src2. */
    if (handler.imm_handler && ot(handler.imm_handler(dest_reg, src1_reg, op->src2.c.i, flags, ENFORCE_ENCODING_NONE)))
    {
      if (src1_alloc.reg != 0)
        restore_scratch_reg(&src1_alloc);
      return;
    }

    src2_alloc = get_scratch_reg_with_save(exclude_regs);
    src2_reg = src2_alloc.reg;
    load_to_reg(src2_reg, PREG_NONE, &op->src2);
  }
  else if (src2_needs_load)
  {
    src2_alloc = get_scratch_reg_with_save(exclude_regs);
    src2_reg = src2_alloc.reg;
    load_to_reg(src2_reg, PREG_NONE, &op->src2);
  }
  else
  {
    thumb_require_materialized_reg(ctx, "src2", src2_reg);
  }

  ot_check(handler.reg_handler(dest_reg, src1_reg, src2_reg, flags, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));

  if (src2_alloc.reg != 0)
    restore_scratch_reg(&src2_alloc);
  if (src1_alloc.reg != 0)
    restore_scratch_reg(&src1_alloc);
}

void tcc_gen_machine_data_processing_op(TACQuadruple *op)
{
  ThumbDataProcessingHandler handler;
  thumb_flags_behaviour flags = FLAGS_BEHAVIOUR_NOT_IMPORTANT;

  /* Check for 64-bit operations */
  if (is_64bit_type(op->dest.type.t))
  {
    return thumb_process_data64_op(op);
  }

  /* NOTE: All spilled register loading is now handled centrally in generate_code via
   * tcc_ir_materialize_value()/materialize_dest(). This function receives valid
   * physical registers in pr0/pr1 (no PREG_SPILLED sentinels). */

  switch (op->op)
  {
  case TCCIR_OP_ADD:
    handler.imm_handler = th_add_imm;
    handler.reg_handler = th_add_reg;
    break;
  case TCCIR_OP_SUB:
    handler.imm_handler = th_sub_imm;
    handler.reg_handler = th_sub_reg;
    break;
  case TCCIR_OP_MUL:
  {
    thumb_emit_mul32(op);
    return;
  }
  case TCCIR_OP_CMP:
    handler.imm_handler = th_cmp_imm;
    handler.reg_handler = th_cmp_reg;
    break;
  case TCCIR_OP_SHL:
  {
    /* Fallback: 32-bit shift handling */
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
    handler.imm_handler = th_orr_imm;
    handler.reg_handler = th_orr_reg;
    break;
  }
  case TCCIR_OP_AND:
  {
    handler.imm_handler = th_and_imm;
    handler.reg_handler = th_and_reg;
    break;
  }
  case TCCIR_OP_XOR:
  {
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
    thumb_emit_regonly_binop32(op, thumb_sdiv_regonly, "DIV");
    return;
  }
  case TCCIR_OP_UDIV:
  {
    thumb_emit_regonly_binop32(op, thumb_udiv_regonly, "UDIV");
    return;
  }
  case TCCIR_OP_IMOD:
  {
    thumb_emit_mod32(op, thumb_sdiv_regonly, "IMOD");
    return;
  }
  case TCCIR_OP_UMOD:
  {
    thumb_emit_mod32(op, thumb_udiv_regonly, "UMOD");
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

    /* Handle immediate constant, missing register, or lvalue (needs dereference).
     * When VT_LVAL is set, the register holds an address and we need to load
     * the value it points to before comparing against zero. */
    int needs_load = th_has_immediate_value(op->src1.r) || src_reg == PREG_NONE || (op->src1.r & VT_LVAL);
    if (needs_load)
    {
      src_alloc = get_scratch_reg_with_save(0);
      src_reg = src_alloc.reg;
      load_to_reg(src_reg, PREG_NONE, &op->src1);
    }
    else
    {
      thumb_require_materialized_reg("TEST_ZERO", "src", src_reg);
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
    return;
  }
  }

  thumb_emit_data_processing_op32(op, handler, flags);
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
    /* IR owns spills: destination must be either a real register pair or a true memory lvalue. */
    if (dest->pr0 != PREG_NONE || dest->pr1 != PREG_NONE)
    {
      if (dest->pr0 == PREG_NONE || dest->pr1 == PREG_NONE)
        tcc_error("compiler_error: hard-float double result destination missing register half");
      thumb_require_materialized_reg("store_fp_result_from_vfp", "dest.low", dest->pr0);
      thumb_require_materialized_reg("store_fp_result_from_vfp", "dest.high", dest->pr1);

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
    else if (dest->r & VT_LVAL)
    {
      /* Store both words to memory as two 32-bit stores. */
      SValue dest_low = *dest;
      SValue dest_high = *dest;
      dest_low.type.t = VT_INT;
      dest_high.type.t = VT_INT;
      dest_high.c.i += 4;
      store(R0, &dest_low);
      store(R1, &dest_high);
    }
    else
    {
      tcc_error("compiler_error: hard-float double result destination is neither register nor memory lvalue");
    }
  }
  else
  {
    ot_check(th_vmov_gp_sp(R0, result_sreg, 1 /* to ARM */));
    /* IR owns spills: destination must be either a real register or a true memory lvalue. */
    if (dest->pr0 != PREG_NONE)
    {
      thumb_require_materialized_reg("store_fp_result_from_vfp", "dest", dest->pr0);
      if (dest->pr0 != R0)
      {
        ot_check(th_mov_reg(dest->pr0, R0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                            false));
      }
    }
    else if (dest->r & VT_LVAL)
    {
      store(R0, dest);
    }
    else
    {
      tcc_error("compiler_error: hard-float float result destination is neither register nor memory lvalue");
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
    Sym *sym = (q->src1.r & VT_SYM) ? q->src1.sym : NULL;
    tcc_machine_load_constant(R0, is_64bit ? R1 : PREG_NONE, q->src1.c.i, is_64bit, sym);
    return;
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
  const char *ctx = "tcc_gen_machine_store_op";
  int src_reg;
  /* Check for 64-bit types - include VT_LLONG for soft-float doubles and long
   * long */
  int src_btype = op->src1.type.t & VT_BTYPE;
  int is_64bit = (src_btype == VT_DOUBLE) || (src_btype == VT_LDOUBLE) || (src_btype == VT_LLONG);

  src_reg = op->src1.pr0;
  ScratchRegAlloc scratch_alloc = {0};

  /* If src_reg is missing, spilled, or src1 isn't a direct register value (const/lvalue), reload it. */
  const int src_is_const = ((op->src1.r & VT_VALMASK) == VT_CONST);
  const int src_is_lval = (op->src1.r & VT_LVAL) != 0;
  const int src_is_spilled = (src_reg != PREG_NONE) && (src_reg & PREG_SPILLED);
  const int need_reload = (src_reg == PREG_NONE) || src_is_spilled || src_is_const || src_is_lval;

  /* IR owns spills: after checking need_reload, assert that non-reloaded sources are materialized. */
  if (!need_reload && src_reg != PREG_NONE)
    thumb_require_materialized_reg(ctx, "src.low", src_reg);

  if (need_reload)
  {
    /* For 64-bit reloads we use R11 as the high word; keep it out of the low scratch choice. */
    const uint32_t exclude = is_64bit ? (1u << R11) : 0;
    scratch_alloc = get_scratch_reg_with_save(exclude);
    src_reg = scratch_alloc.reg;
    load_to_reg(src_reg, is_64bit ? R11 : PREG_NONE, &op->src1);

    SValue store_dest = op->dest;
    if (is_64bit)
      store_dest.pr1 = R11;
    store(src_reg, &store_dest);
  }
  else
  {
    SValue store_dest = op->dest;
    if (is_64bit)
    {
      store_dest.pr1 = op->src1.pr1;
      if (store_dest.pr1 != PREG_NONE)
        thumb_require_materialized_reg(ctx, "src.high", store_dest.pr1);
    }
    store(src_reg, &store_dest);
  }

  if (scratch_alloc.saved || scratch_alloc.reg >= 0)
    restore_scratch_reg(&scratch_alloc);
}

ST_FUNC void tcc_gen_machine_prolog(int leaffunc, uint64_t used_registers, int stack_size)
{
  thumb_gen_state.function_argument_count = 0;
  /* call_id -1 is reserved for function prolog metadata - but that doesn't
   * need to be stored in call_sites_by_id (which uses non-negative IDs).
   * If needed, handle it separately or skip. */

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
  /* Keep the total push size 8-byte aligned (AAPCS). This must not be done by
   * adding padding below SP (would shift prepared-call stack arguments). */
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
    gadd_sp(-stack_size);
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

    /* NOTE: Do not hard-code small fixed arrays here.
     * Functions can legally have >32 parameters (e.g. sum40 in tests), and
     * overflowing these buffers corrupts prolog codegen and breaks calls.
     * Worst-case: a 64-bit param can contribute up to 2 reg moves.
     */
    const int max_param_moves = ir->next_parameter * 2 + 8;
    ParamMove *moves = tcc_malloc(sizeof(ParamMove) * max_param_moves);
    int move_count = 0;

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
        /* Stack-passed parameters live permanently in the caller's argument
         * area. Leave their allocations empty so IR materialization can treat
         * them as VT_PARAM lvalues and load directly when needed. */
        interval->allocation.r0 = PREG_NONE;
        interval->allocation.r1 = PREG_NONE;
        interval->allocation.offset = 0;
        continue;
      }

      /* Stack-home parameters: store incoming regs to their stack slots.
       * IR owns spills; avoid inspecting PREG_SPILLED sentinels here.
       */
      if (interval->allocation.offset != 0)
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

    /* Execute collected register moves as a true parallel move.
     *
     * - Emit any move whose source is not a destination.
     * - If only cycles remain, rotate a cycle using a temporary register.
     *
     * This is required for correct swaps like (r1<-r2, r2<-r1) without
     * clobbering one of the incoming argument registers.
     */
    while (move_count > 0)
    {
      uint32_t dst_mask = 0;
      uint32_t src_mask = 0;
      for (int i = 0; i < move_count; ++i)
      {
        if (moves[i].dst >= 0 && moves[i].dst < 32)
          dst_mask |= (1u << moves[i].dst);
        if (moves[i].src >= 0 && moves[i].src < 32)
          src_mask |= (1u << moves[i].src);
      }

      /* First: emit all acyclic moves.
       *
       * A move is safe to emit if its destination is not used as a source by
       * any remaining move. This prevents clobbering values needed later.
       *
       * Example chain that must be ordered correctly:
       *   r1 <- r2
       *   r2 <- r3
       * Here r2 is both a source and a destination. We must emit r1<-r2 first.
       */
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

        if (dst >= 0 && dst < 32 && (src_mask & (1u << dst)))
          continue; /* dst's current value is still needed as a source somewhere */

        ot_check(
            th_mov_reg(dst, src, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
        moves[i] = moves[--move_count];
        --i;
        progressed = 1;
      }
      if (progressed)
        continue;

      /* Cycle: rotate it using a temporary register (prefer IP). */
      int temp = R_IP;
      if (dst_mask & (1u << temp))
      {
        for (int r = R4; r <= R11; ++r)
        {
          if (!(dst_mask & (1u << r)))
          {
            temp = r;
            break;
          }
        }
      }
      if (dst_mask & (1u << temp))
      {
        tcc_error("compiler_error: prolog param shuffle has no temp register");
      }

      /* Pick any destination in the remaining cycle, save its original value,
       * then walk dst<-src edges until we return to the start.
       */
      const int start = moves[0].dst;
      ot_check(
          th_mov_reg(temp, start, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));

      int cur = start;
      for (;;)
      {
        int idx = -1;
        for (int i = 0; i < move_count; ++i)
        {
          if (moves[i].dst == cur)
          {
            idx = i;
            break;
          }
        }
        if (idx < 0)
        {
          tcc_error("compiler_error: broken prolog param shuffle cycle");
        }

        const int src = moves[idx].src;
        moves[idx] = moves[--move_count];

        if (src == start)
        {
          ot_check(
              th_mov_reg(cur, temp, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
          break;
        }

        ot_check(
            th_mov_reg(cur, src, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
        cur = src;
      }
    }

    tcc_free(moves);
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
    gadd_sp(allocated_stack_size);
  }

  if (lr_saved)
  {
    pushed_registers |= 1 << R_PC;
    pushed_registers &= ~(1 << R_LR);
    ot_check(th_pop(pushed_registers));
    thumb_gen_state.generating_function = 0;
    th_literal_pool_generate();
    thumb_free_call_sites();

    return;
  }
  if (pushed_registers > 0)
  {
    ot_check(th_pop(pushed_registers));
  }
  thumb_gen_state.generating_function = 0;
  ot_check(th_bx_reg(R_LR));
  th_literal_pool_generate();

  thumb_free_call_sites();
}

ST_FUNC void tcc_gen_machine_assign_op(TACQuadruple *op)
{
  const char *ctx = "tcc_gen_machine_assign_op";
  /* Only consider VFP registers if hard float ABI is enabled */
  int use_vfp_regs = (tcc_state->float_abi == ARM_HARD_FLOAT);
  int dest_is_vfp = use_vfp_regs && LS_IS_VFP_REG(op->dest.pr0);
  int src_is_vfp = use_vfp_regs && LS_IS_VFP_REG(op->src1.pr0);
  /* For ASSIGN, the destination type defines the operation width.
   * A 64-bit->32-bit assignment is a narrowing conversion (low word only).
   * Treating it as a 64-bit move can try to write a non-existent high reg
   * (often PREG_NONE=0xFF), which encodes as PC and generates invalid code.
   */
  const int dest_is_64bit = is_64bit_type(op->dest.type.t);
  int dest_is_local = (op->dest.r & VT_VALMASK) == VT_LOCAL;

  /* NOTE: Avoid noisy debug prints in normal builds. */

  /* NOTE: Spilled operands (sources and destinations) are materialized in
   * tcc_ir_generate_code() before we get here, so src1/dest already name
   * concrete registers or true memory lvalues. */

  if (dest_is_64bit)
  {
    /* 64-bit assign/move must preserve both low and high words.
     * This is critical for switch-range lowering which spills 64-bit
     * temporaries to the stack and later reloads them for __aeabi_lcmp.
     */
    /* Only treat true lvalues as memory destinations here.
     * Spilled vregs have already been materialized into registers by IR. */
    const int dest_in_mem = (op->dest.r & VT_LVAL) != 0;

    int src_lo = op->src1.pr0;
    int src_hi = op->src1.pr1;
    ScratchRegAlloc src_lo_alloc = {0};
    ScratchRegAlloc src_hi_alloc = {0};

    /* Check for spilled sources - these need to be loaded to registers */
    const int src_lo_spilled = (src_lo != PREG_NONE) && (src_lo & PREG_SPILLED);
    const int src_hi_spilled = (src_hi != PREG_NONE) && (src_hi & PREG_SPILLED);

    /* Materialize source into registers if needed (const/spilled/lvalue/etc).
     * If either half is spilled, reload the whole 64-bit value. */
    if ((op->src1.r & VT_VALMASK) == VT_CONST || (op->src1.r & VT_LVAL) || src_lo == PREG_NONE || src_lo_spilled ||
        src_hi_spilled)
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
    else if (src_hi == PREG_NONE)
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

  if ((op->src1.r & VT_VALMASK) == VT_CONST && !(op->src1.r & VT_LVAL))
  {
    /* Pure constant (not a memory dereference). Use machine API directly. */
    Sym *sym = (op->src1.r & VT_SYM) ? op->src1.sym : NULL;
    int is_64bit = dest_is_64bit;

    if (dest_is_vfp)
    {
      int dn = LS_VFP_REG_NUM(op->dest.pr0);
      /* Load constant to integer register, then move to VFP */
      ScratchRegAlloc scratch_alloc = get_scratch_reg_with_save(0);
      int scratch_reg = scratch_alloc.reg;
      tcc_machine_load_constant(scratch_reg, PREG_NONE, op->src1.c.i, 0, sym);
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
      tcc_machine_load_constant(scratch_reg, PREG_NONE, op->src1.c.i, 0, sym);
      SValue dest_direct = op->dest;
      dest_direct.r &= ~VT_LVAL; /* Clear VT_LVAL - we want direct store to stack offset */
      store(scratch_reg, &dest_direct);
      restore_scratch_reg(&scratch_alloc);
    }
    else
    {
      tcc_machine_load_constant(op->dest.pr0, is_64bit ? op->dest.pr1 : PREG_NONE, op->src1.c.i, is_64bit, sym);
    }
    return;
  }

  /* VT_CONST with VT_LVAL means dereference a global symbol - use load_to_reg */
  if ((op->src1.r & VT_VALMASK) == VT_CONST && (op->src1.r & VT_LVAL))
  {
    if (dest_is_vfp)
    {
      int dn = LS_VFP_REG_NUM(op->dest.pr0);
      ScratchRegAlloc scratch_alloc = get_scratch_reg_with_save(0);
      int scratch_reg = scratch_alloc.reg;
      load_to_reg(scratch_reg, PREG_NONE, &op->src1);
      ot_check(th_vmov_gp_sp(scratch_reg, dn, 0));
      restore_scratch_reg(&scratch_alloc);
    }
    else if ((op->dest.r & VT_LVAL) && ((op->dest.r & VT_VALMASK) == VT_LOCAL || (op->dest.r & VT_VALMASK) == VT_CONST))
    {
      ScratchRegAlloc scratch_alloc = get_scratch_reg_with_save(0);
      int scratch_reg = scratch_alloc.reg;
      load_to_reg(scratch_reg, PREG_NONE, &op->src1);
      SValue dest_direct = op->dest;
      dest_direct.r &= ~VT_LVAL;
      store(scratch_reg, &dest_direct);
      restore_scratch_reg(&scratch_alloc);
    }
    else
    {
      load_to_reg(op->dest.pr0, dest_is_64bit ? op->dest.pr1 : PREG_NONE, &op->src1);
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

  /* NOTE: Writeback to memory for spilled destinations is handled by the
   * IR-side materialize_dest/storeback path. We should NOT do an inline
   * writeback here because:
   * 1. Spilled temporaries (TMP vregs) are handled centrally
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
  const char *ctx = "tcc_gen_machine_lea_op";
  int dest_reg = op->dest.pr0;
  int src_v = op->src1.r & VT_VALMASK;

  /* IR owns spills: LEA destination must already be materialized. */
  thumb_require_materialized_reg(ctx, "dest", dest_reg);

  if (src_v == VT_LOCAL || src_v == VT_LLOCAL)
  {
    /* Compute address of local: FP + offset */
    int base = R_FP;
    if (tcc_state->need_frame_pointer == 0)
      base = R_SP;

    /* Use vreg-based stack slot offset if available, otherwise fall back to c.i */
    int offset;
    const TCCStackSlot *slot = tcc_ir_stack_slot_by_vreg(tcc_state->ir, op->src1.vr);
    if (slot)
      offset = slot->offset;
    else
      offset = (int)op->src1.c.i;
    /* Stack parameters live above the saved-register area.
     * When computing their address, fold in offset_to_args (prologue push size). */
    if (op->src1.r & VT_PARAM)
      offset += offset_to_args;
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
    if (src_reg != PREG_NONE)
    {
      thumb_require_materialized_reg(ctx, "src", src_reg);
      if (src_reg != dest_reg)
      {
        ot_check(th_mov_reg(dest_reg, src_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE, false));
      }
    }
    else
    {
      tcc_error("compiler_error: LEA on unexpected operand type r=0x%x", op->src1.r);
    }
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

/* Store a register to a stack slot relative to SP.
 * Used for outgoing call arguments (stack args must be located at SP at call time).
 * offset is expected to be non-negative.
 */
ST_FUNC void tcc_gen_machine_store_to_sp(int reg, int offset)
{
  int sign = (offset < 0);
  int abs_offset = sign ? -offset : offset;

  if (!store_word_to_base(reg, R_SP, abs_offset, sign))
  {
    ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(abs_offset, sign, (1u << reg) | (1u << R_SP));
    int rr = rr_alloc.reg;
    ot_check(th_str_reg(reg, R_SP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
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
    /* Indirect call through register.
     *
     * When the target type is VT_FUNC (direct function designator), if the
     * address already lives in a register (v < VT_CONST), clear VT_LVAL so
     * we don't emit a bogus extra load like "ldr ip, [ip]" before blx.
     *
     * BUT: When we have a pointer-to-function (bt == VT_PTR pointing to VT_FUNC),
     * VT_LVAL means the register holds the ADDRESS where the function pointer
     * is stored (e.g., from array access tabl[i]), and we DO need to load from it.
     */
    int bt = dest->type.t & VT_BTYPE;
    if (bt == VT_FUNC)
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
  const char *ctx = "load_to_register";

  if ((sv->r & VT_VALMASK) == VT_LOCAL)
  {
    /* VT_LOCAL without VT_LVAL means we need the ADDRESS of the local variable.
     * In this case we must compute FP + offset, not do a register move. */
    if (!(sv->r & VT_LVAL))
    {
      int r1 = (sv->pr1 != PREG_NONE && is_64bit_type(sv->type.t)) ? sv->pr1 : PREG_NONE;
      load_to_reg(reg, r1, sv);
      return;
    }

    if (sv->pr0 != PREG_NONE)
    {
      int cached = (reg_from != PREG_NONE) ? reg_from : sv->pr0;
      thumb_require_materialized_reg(ctx, "cached local value", cached);
      if (reg != cached)
      {
        ot_check(
            th_mov_reg(reg, cached, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
      }
      return;
    }

    /* Local spilled to stack - reload */
    int r1 = (sv->pr1 != PREG_NONE && is_64bit_type(sv->type.t)) ? sv->pr1 : PREG_NONE;
    load_to_reg(reg, r1, sv);
    return;
  }

  if ((sv->r & VT_LVAL) || sv->pr0 == PREG_NONE)
  {
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
  thumb_require_materialized_reg(ctx, "source register", src_reg);
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

static void tcc_thumb_ir_store_u32_to_sp(TCCIRState *ir, int off, SValue *val)
{
  (void)ir;
  TCCMachineScratchRegs scratch = {0};
  tcc_machine_acquire_scratch(&scratch, TCC_MACHINE_SCRATCH_AVOID_CALL_ARG_REGS);
  int rtmp = scratch.regs[0];
  load_to_reg(rtmp, PREG_NONE, val);
  tcc_gen_machine_store_to_sp(rtmp, off);
  tcc_machine_release_scratch(&scratch);
}

static void tcc_thumb_ir_store_u64_to_sp(TCCIRState *ir, int off, SValue *val)
{
  (void)ir;
  TCCMachineScratchRegs scratch = {0};
  tcc_machine_acquire_scratch(&scratch, TCC_MACHINE_SCRATCH_NEEDS_PAIR | TCC_MACHINE_SCRATCH_AVOID_CALL_ARG_REGS);
  int rlo = scratch.regs[0];
  int rhi = scratch.regs[1];
  load_to_reg(rlo, rhi, val);
  tcc_gen_machine_store_to_sp(rlo, off);
  tcc_gen_machine_store_to_sp(rhi, off + 4);
  tcc_machine_release_scratch(&scratch);
}

static void tcc_thumb_ir_copy_struct_to_sp(int dst_off, const SValue *sv, int size)
{
  const int slot_sz = tcc_abi_align_up_int(size, 4);
  TCCMachineScratchRegs scratch = {0};
  /* Need base+tmp. Avoid call arg regs. */
  tcc_machine_acquire_scratch(&scratch, TCC_MACHINE_SCRATCH_NEEDS_PAIR | TCC_MACHINE_SCRATCH_AVOID_CALL_ARG_REGS);
  int base = scratch.regs[0];
  int tmp = scratch.regs[1];

  SValue addr = *sv;
  addr.r &= ~VT_LVAL;
  addr.type.t = VT_PTR;
  load_to_reg(base, PREG_NONE, &addr);

  /* Copy full words */
  int copied = 0;
  while (copied + 4 <= size)
  {
    if (!load_word_from_base(tmp, base, copied, 0))
    {
      ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(
          copied, 0, (1u << tmp) | (1u << base) | (1u << R_SP) | (1u << R0) | (1u << R1) | (1u << R2) | (1u << R3));
      int rr = rr_alloc.reg;
      ot_check(th_ldr_reg(tmp, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      restore_scratch_reg(&rr_alloc);
    }
    tcc_gen_machine_store_to_sp(tmp, dst_off + copied);
    copied += 4;
  }

  /* Tail bytes */
  if (copied < size)
  {
    for (; copied < size; ++copied)
    {
      if (!load_ubyte_from_base(tmp, base, copied, 0))
      {
        ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(
            copied, 0, (1u << tmp) | (1u << base) | (1u << R_SP) | (1u << R0) | (1u << R1) | (1u << R2) | (1u << R3));
        int rr = rr_alloc.reg;
        ot_check(th_ldrb_reg(tmp, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&rr_alloc);
      }
      /* store byte tmp -> [sp + dst_off + copied] */
      const int off = dst_off + copied;
      if (!ot(th_strb_imm(tmp, R_SP, off, 6, ENFORCE_ENCODING_NONE)))
      {
        ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(
            off, 0, (1u << tmp) | (1u << R_SP) | (1u << base) | (1u << R0) | (1u << R1) | (1u << R2) | (1u << R3));
        int rr = rr_alloc.reg;
        ot_check(th_strb_reg(tmp, R_SP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&rr_alloc);
      }
    }
  }

  /* Zero-pad to slot size */
  if (slot_sz > size)
  {
    ot_check(th_eor_reg(tmp, tmp, tmp, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    for (int off = size; off < slot_sz; ++off)
    {
      const int sp_off = dst_off + off;
      if (!ot(th_strb_imm(tmp, R_SP, sp_off, 6, ENFORCE_ENCODING_NONE)))
      {
        ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(
            sp_off, 0, (1u << tmp) | (1u << R_SP) | (1u << base) | (1u << R0) | (1u << R1) | (1u << R2) | (1u << R3));
        int rr = rr_alloc.reg;
        ot_check(th_strb_reg(tmp, R_SP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&rr_alloc);
      }
    }
  }

  tcc_machine_release_scratch(&scratch);
}

/* Load a 32-bit immediate value or symbol address into a register.
 * Uses th_generic_mov_imm if possible (pure immediates only), otherwise loads from literal pool.
 * reg: target register
 * imm: 32-bit immediate value or offset to load
 * sym: symbol reference (NULL for pure immediates)
 * update_flags: whether the load should update condition flags (currently unused)
 */
static void load_immediate(int reg, uint32_t imm, Sym *sym, int update_flags)
{
  (void)update_flags; /* Currently not used, reserved for future use */

  /* If there's a symbol, always use literal pool for relocations */
  if (sym)
  {
    load_full_const(reg, PREG_NONE, imm, sym);
    return;
  }

  /* Try to encode as ARM immediate (supports various rotated 8-bit patterns) */
  if (!ot(th_generic_mov_imm(reg, imm)))
  {
    /* Value doesn't fit in immediate encoding, use literal pool */
    load_full_const(reg, PREG_NONE, imm, NULL);
  }
}

typedef enum ThumbArgMoveKind
{
  THUMB_ARG_MOVE_REG,
  THUMB_ARG_MOVE_IMM,
  THUMB_ARG_MOVE_IMM64,      /* load 64-bit immediate into register pair */
  THUMB_ARG_MOVE_LOCAL_ADDR, /* compute address of local: fp + offset */
  THUMB_ARG_MOVE_LVAL,       /* load from memory (lvalue) */
  THUMB_ARG_MOVE_STRUCT,     /* load struct words into consecutive registers */
} ThumbArgMoveKind;

typedef struct ThumbArgMove
{
  ThumbArgMoveKind kind;
  int dst_reg;
  int dst_reg_hi;        /* valid when kind==THUMB_ARG_MOVE_IMM64 */
  int src_reg;           /* valid when kind==THUMB_ARG_MOVE_REG */
  uint32_t imm;          /* valid when kind==THUMB_ARG_MOVE_IMM */
  uint64_t imm64;        /* valid when kind==THUMB_ARG_MOVE_IMM64 */
  Sym *sym;              /* valid when kind==THUMB_ARG_MOVE_IMM */
  int local_offset;      /* valid when kind==THUMB_ARG_MOVE_LOCAL_ADDR */
  int local_is_param;    /* valid when kind==THUMB_ARG_MOVE_LOCAL_ADDR - if true, add offset_to_args */
  SValue lval_sv;        /* valid when kind==THUMB_ARG_MOVE_LVAL */
  int struct_word_count; /* valid when kind==THUMB_ARG_MOVE_STRUCT */
} ThumbArgMove;

static void thumb_emit_arg_move(const ThumbArgMove *m)
{
  if (m->kind == THUMB_ARG_MOVE_REG)
  {
    if (m->src_reg == m->dst_reg)
      return;
    ot_check(th_mov_reg(m->dst_reg, m->src_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                        ENFORCE_ENCODING_NONE, false));
    return;
  }

  if (m->kind == THUMB_ARG_MOVE_LOCAL_ADDR)
  {
    /* Compute address of local variable: dst = fp + offset */
    tcc_machine_addr_of_stack_slot(m->dst_reg, m->local_offset, m->local_is_param);
    return;
  }

  if (m->kind == THUMB_ARG_MOVE_LVAL)
  {
    /* Load value from memory (lvalue) */
    SValue sv_copy = m->lval_sv;
    /* Use dst_reg_hi for 64-bit types (double, long long) */
    int hi_reg = (tcc_is_64bit_type(sv_copy.type.t) && m->dst_reg_hi != 0) ? m->dst_reg_hi : PREG_NONE;
    load_to_reg(m->dst_reg, hi_reg, &sv_copy);
    return;
  }

  if (m->kind == THUMB_ARG_MOVE_STRUCT)
  {
    /* Load struct words into consecutive registers.
     * The lval_sv contains the struct address. */
    SValue sv_copy = m->lval_sv;
    int word_count = m->struct_word_count;
    int base_dst = m->dst_reg;

    /* Get the struct base address into a scratch register */
    int base_addr_reg = ARM_R12;

    if ((sv_copy.r & VT_VALMASK) == VT_LOCAL)
    {
      /* Local struct - compute FP + offset */
      int local_off = (int)sv_copy.c.i;
      int is_param = (sv_copy.r & VT_PARAM) ? 1 : 0;
      tcc_machine_addr_of_stack_slot(base_addr_reg, local_off, is_param);
    }
    else if ((sv_copy.r & VT_VALMASK) == VT_CONST && (sv_copy.r & VT_SYM))
    {
      /* Global struct */
      load_immediate(base_addr_reg, (uint32_t)sv_copy.c.i, sv_copy.sym, false);
    }
    else if (sv_copy.pr0 != PREG_NONE && !(sv_copy.pr0 & PREG_SPILLED))
    {
      /* Address already in a register */
      base_addr_reg = sv_copy.pr0;
    }
    else
    {
      /* Fallback: try to compute struct address */
      SValue addr_sv = sv_copy;
      addr_sv.r &= ~VT_LVAL; /* we want the address, not the value */
      load_to_reg(base_addr_reg, PREG_NONE, &addr_sv);
    }

    /* Load each word from the struct into consecutive target registers */
    for (int w = 0; w < word_count; ++w)
    {
      int dst = base_dst + w;
      int offset = w * 4;
      if (!load_word_from_base(dst, base_addr_reg, offset, 0))
      {
        /* Large offset - use R12 as scratch if it's not our base */
        if (base_addr_reg != ARM_R12)
        {
          load_immediate(ARM_R12, offset, NULL, false);
          ot_check(th_ldr_reg(dst, base_addr_reg, ARM_R12, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        }
        else
        {
          /* base_addr_reg is R12, need another approach */
          load_immediate(ARM_LR, offset, NULL, false);
          ot_check(th_ldr_reg(dst, base_addr_reg, ARM_LR, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        }
      }
    }
    return;
  }

  if (m->kind == THUMB_ARG_MOVE_IMM64)
  {
    /* Load 64-bit immediate into register pair */
    uint32_t lo = (uint32_t)(m->imm64 & 0xFFFFFFFF);
    uint32_t hi = (uint32_t)(m->imm64 >> 32);
    load_immediate(m->dst_reg, lo, NULL, false);
    load_immediate(m->dst_reg_hi, hi, NULL, false);
    return;
  }

  /* THUMB_ARG_MOVE_IMM */
  load_immediate(m->dst_reg, m->imm, m->sym, false);
}

/* Schedule register argument setup as a parallel assignment.
 * This avoids clobbering a source register needed for another argument.
 * Example: r0 <- r6, r1 <- r0 must be emitted as:
 *   mov r1, r0
 *   mov r0, r6
 */
static void thumb_emit_parallel_arg_moves(ThumbArgMove *moves, int move_count)
{
  if (move_count <= 0)
    return;

  uint8_t done[16];
  memset(done, 0, sizeof(done));

  ScratchRegAlloc tmp_alloc = (ScratchRegAlloc){0};
  int have_tmp = 0;

  for (int remaining = move_count; remaining > 0;)
  {
    uint32_t src_set = 0;
    for (int i = 0; i < move_count; ++i)
    {
      if (done[i])
        continue;
      if (moves[i].kind == THUMB_ARG_MOVE_REG)
        src_set |= (1u << moves[i].src_reg);
    }

    int chosen = -1;
    for (int i = 0; i < move_count; ++i)
    {
      if (done[i])
        continue;
      if ((src_set & (1u << moves[i].dst_reg)) == 0)
      {
        chosen = i;
        break;
      }
    }

    if (chosen < 0)
    {
      /* Cycle among register moves. Break it with a scratch temp. */
      int cyc = -1;
      for (int i = 0; i < move_count; ++i)
      {
        if (!done[i] && moves[i].kind == THUMB_ARG_MOVE_REG)
        {
          cyc = i;
          break;
        }
      }
      if (cyc < 0)
        tcc_error("compiler_error: arg move cycle without reg sources");

      if (!have_tmp)
      {
        /* Exclude all regs involved in the parallel move. */
        uint32_t exclude = 0;
        for (int i = 0; i < move_count; ++i)
        {
          if (done[i])
            continue;
          exclude |= (1u << moves[i].dst_reg);
          if (moves[i].kind == THUMB_ARG_MOVE_REG)
            exclude |= (1u << moves[i].src_reg);
        }
        /* Also exclude SP/PC. */
        exclude |= (1u << ARM_SP) | (1u << ARM_PC);
        tmp_alloc = get_scratch_reg_with_save(exclude);
        have_tmp = 1;
      }

      thumb_require_materialized_reg("thumb_emit_parallel_arg_moves", "tmp", tmp_alloc.reg);
      ot_check(th_mov_reg(tmp_alloc.reg, moves[cyc].src_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE, false));
      moves[cyc].src_reg = tmp_alloc.reg;
      continue;
    }

    thumb_emit_arg_move(&moves[chosen]);
    done[chosen] = 1;
    --remaining;
  }

  if (have_tmp && tmp_alloc.saved)
    ot_check(th_pop(1u << tmp_alloc.reg));
}

ST_FUNC void tcc_gen_machine_func_call_op(TACQuadruple *q, int drop_result, TCCIRState *ir, int call_idx)
{
  if (!q || !ir)
    tcc_error("compiler_error: func_call_op requires q+ir");

  /* Get call_id and argc from src2.c.i (keeps call/param binding explicit). */
  const int call_id = TCCIR_DECODE_CALL_ID(q->src2.c.i);
  const int argc_hint = TCCIR_DECODE_CALL_ARGC(q->src2.c.i);

  /* Get the cached call site created during FUNCPARAMVAL processing */
  ThumbGenCallSite *call_site = thumb_get_call_site_for_id(call_id);
  if (!call_site)
    tcc_error("compiler_error: no call site found for call_id=%d", call_id);

  /* Build ABI call layout using tccabi */
  TCCAbiCallLayout layout;
  memset(&layout, 0, sizeof(layout));

  /* Single scan: get both ABI layout AND argument SValues */
  SValue *args = NULL;
  const int argc = thumb_build_call_layout_from_ir(ir, call_idx, call_id, argc_hint, &layout, &args);
  if (argc < 0)
    tcc_error("compiler_error: failed to build call layout for call_id=%d", call_id);

  /* Calculate total stack space needed */
  const int stack_size = (argc > 0) ? (int)layout.stack_size : 0;

  /* Step 1: Check if any argument registers (R0-R3) are currently in use
   * If we have a nested call, we need to preserve them */
  int arg_regs_in_use = 0;
  for (int reg = ARM_R0; reg <= ARM_R3; reg++)
  {
    if (call_site->registers_map & (1 << reg))
    {
      arg_regs_in_use |= (1 << reg);
    }
  }

  /* Step 2: Push argument registers that are in use (nested call case) */
  if (arg_regs_in_use != 0)
  {
    uint16_t push_mask = (uint16_t)arg_regs_in_use;
    ot_check(th_push(push_mask));
    call_site->used_stack_size += __builtin_popcount(arg_regs_in_use) * 4;
  }

  /* Step 3: Reserve stack space for stack arguments */
  if (stack_size > 0)
  {
    gadd_sp(-stack_size);
    call_site->used_stack_size += stack_size;
  }

  /* Step 4: Place arguments according to ABI layout */
  /* CRITICAL: Block R0-R3 from scratch allocation during argument setup.
   * Without this, get_scratch_reg_with_save() may return R0-R3 as "free"
   * scratch registers, which then get used to load intermediate values
   * (e.g., array base addresses), clobbering the argument registers before
   * the call is emitted. */
  uint32_t saved_scratch_exclude = scratch_global_exclude;
  scratch_global_exclude |= (1u << ARM_R0) | (1u << ARM_R1) | (1u << ARM_R2) | (1u << ARM_R3);

  /* First build a safe parallel move list for R0-R3 arguments.
   * This prevents clobbering sources when multiple arguments originate
   * from overlapping registers (common with nested calls). */
  ThumbArgMove reg_moves[8];
  int reg_move_count = 0;

  for (int i = 0; i < argc; ++i)
  {
    const TCCAbiArgLoc *loc = &layout.locs[i];
    const SValue *arg = &args[i];
    const int bt = arg->type.t & VT_BTYPE;
    const int is_64bit = tcc_is_64bit_type(arg->type.t);

    if (loc->kind != TCC_ABI_LOC_REG)
      continue;

    int base_reg = ARM_R0 + loc->reg_base;

    if (bt == VT_STRUCT)
    {
      /* Load struct words into consecutive registers */
      int words = (loc->size + 3) / 4;
      if (words > 0 && words <= 4)
      {
        /* Add a struct move to load words into R0-R3 */
        reg_moves[reg_move_count++] = (ThumbArgMove){
            .kind = THUMB_ARG_MOVE_STRUCT,
            .dst_reg = base_reg,
            .lval_sv = *arg,
            .struct_word_count = words,
        };
      }
      for (int w = 0; w < words && w < loc->reg_count; w++)
        call_site->registers_map |= (1 << (base_reg + w));
      continue;
    }

    if (is_64bit)
    {
      /* Check for lvalue first - if VT_LVAL is set, we need to load from memory,
       * regardless of whether pr0/pr1 are set (they'd hold the address, not the value) */
      if (arg->r & VT_LVAL)
      {
        /* Load value from memory (lvalue dereference) */
        SValue sv_copy = *arg;
        reg_moves[reg_move_count++] = (ThumbArgMove){
            .kind = THUMB_ARG_MOVE_LVAL, .dst_reg = base_reg, .dst_reg_hi = base_reg + 1, .lval_sv = sv_copy};
      }
      else if (arg->pr0 != PREG_NONE && arg->pr1 != PREG_NONE)
      {
        if (arg->pr0 != base_reg)
          reg_moves[reg_move_count++] =
              (ThumbArgMove){.kind = THUMB_ARG_MOVE_REG, .dst_reg = base_reg, .src_reg = arg->pr0};
        if (arg->pr1 != (base_reg + 1))
          reg_moves[reg_move_count++] =
              (ThumbArgMove){.kind = THUMB_ARG_MOVE_REG, .dst_reg = base_reg + 1, .src_reg = arg->pr1};
      }
      else if ((arg->r & VT_VALMASK) == VT_CONST)
      {
        /* 64-bit constant - load into register pair */
        reg_moves[reg_move_count++] = (ThumbArgMove){
            .kind = THUMB_ARG_MOVE_IMM64, .dst_reg = base_reg, .dst_reg_hi = base_reg + 1, .imm64 = arg->c.i};
      }
      else
      {
        /* Fallback: use load_to_reg for other 64-bit cases */
        SValue sv_copy = *arg;
        reg_moves[reg_move_count++] = (ThumbArgMove){
            .kind = THUMB_ARG_MOVE_LVAL, .dst_reg = base_reg, .dst_reg_hi = base_reg + 1, .lval_sv = sv_copy};
      }
      call_site->registers_map |= (1 << base_reg);
      call_site->registers_map |= (1 << (base_reg + 1));
      continue;
    }

    /* 32-bit scalar */
    if (arg->r & VT_LVAL)
    {
      /* Load value from memory (lvalue dereference) - must check this FIRST
       * because a dereferenced pointer can still have pr0 set (holding the address) */
      reg_moves[reg_move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_LVAL, .dst_reg = base_reg, .lval_sv = *arg};
    }
    else if (arg->pr0 != PREG_NONE)
    {
      if (arg->pr0 != base_reg)
        reg_moves[reg_move_count++] =
            (ThumbArgMove){.kind = THUMB_ARG_MOVE_REG, .dst_reg = base_reg, .src_reg = arg->pr0};
    }
    else if ((arg->r & VT_VALMASK) == VT_CONST)
    {
      uint32_t imm = (uint32_t)arg->c.i;
      Sym *sym = (arg->r & VT_SYM) ? arg->sym : NULL;
      reg_moves[reg_move_count++] =
          (ThumbArgMove){.kind = THUMB_ARG_MOVE_IMM, .dst_reg = base_reg, .imm = imm, .sym = sym};
    }
    else if ((arg->r & VT_VALMASK) == VT_LOCAL && !(arg->r & VT_LVAL))
    {
      /* Address of local variable - compute fp + offset */
      reg_moves[reg_move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_LOCAL_ADDR,
                                                   .dst_reg = base_reg,
                                                   .local_offset = (int)arg->c.i,
                                                   .local_is_param = (arg->r & VT_PARAM) ? 1 : 0};
    }
    else
    {
      /* Fallback: try loading via load_to_reg */
      reg_moves[reg_move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_LVAL, .dst_reg = base_reg, .lval_sv = *arg};
    }

    call_site->registers_map |= (1 << base_reg);
  }

  /* CRITICAL: Before executing register moves, save any stack arguments that
   * source from R0-R3. These registers may be clobbered by the parallel move.
   * For example: if R3 holds 'd' and needs to go to stack, but R3 <- R2 is
   * part of the register shuffle, we must save 'd' first. */
  for (int i = 0; i < argc; ++i)
  {
    const TCCAbiArgLoc *loc = &layout.locs[i];
    const SValue *arg = &args[i];
    const int bt = arg->type.t & VT_BTYPE;
    const int is_64bit = tcc_is_64bit_type(arg->type.t);

    if (loc->kind == TCC_ABI_LOC_REG)
      continue;

    /* Only handle 32-bit scalars with register source in R0-R3 */
    if (bt == VT_STRUCT || is_64bit)
      continue;

    if (arg->pr0 != PREG_NONE && arg->pr0 <= ARM_R3)
    {
      /* This stack argument sources from R0-R3, save it now before the shuffle */
      int stack_offset = loc->stack_off;
      if (!store_word_to_base(arg->pr0, ARM_SP, stack_offset, 0))
      {
        load_immediate(ARM_R12, stack_offset, NULL, false);
        ot_check(th_str_reg(arg->pr0, ARM_SP, ARM_R12, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      }
    }
  }

  thumb_emit_parallel_arg_moves(reg_moves, reg_move_count);

  /* Now handle stack arguments (if any). */
  for (int i = 0; i < argc; ++i)
  {
    const TCCAbiArgLoc *loc = &layout.locs[i];
    const SValue *arg = &args[i];
    const int bt = arg->type.t & VT_BTYPE;
    const int is_64bit = tcc_is_64bit_type(arg->type.t);

    if (loc->kind == TCC_ABI_LOC_REG)
      continue;

    /* TCC_ABI_LOC_STACK */
    {
      /* Argument goes on stack */
      int stack_offset = loc->stack_off;

      if (bt == VT_STRUCT)
      {
        /* Copy struct to stack. Get struct address and copy each word. */
        int struct_size = loc->size; /* use ABI-computed size */
        int words = (struct_size + 3) / 4;
        int base_addr_reg = ARM_R12;

        /* Get the struct base address */
        if ((arg->r & VT_VALMASK) == VT_LOCAL)
        {
          int local_off = (int)arg->c.i;
          int is_param = (arg->r & VT_PARAM) ? 1 : 0;
          tcc_machine_addr_of_stack_slot(base_addr_reg, local_off, is_param);
        }
        else if ((arg->r & VT_VALMASK) == VT_CONST && (arg->r & VT_SYM))
        {
          load_immediate(base_addr_reg, (uint32_t)arg->c.i, arg->sym, false);
        }
        else if (arg->pr0 != PREG_NONE && !(arg->pr0 & PREG_SPILLED))
        {
          base_addr_reg = arg->pr0;
        }
        else
        {
          SValue addr_sv = *arg;
          addr_sv.r &= ~VT_LVAL;
          load_to_reg(base_addr_reg, PREG_NONE, &addr_sv);
        }

        /* Copy each word from struct to stack */
        for (int w = 0; w < words; ++w)
        {
          int src_off = w * 4;
          int dst_off = stack_offset + w * 4;

          /* Load word from struct into LR (use LR as temp since R12 may hold base) */
          if (!load_word_from_base(ARM_LR, base_addr_reg, src_off, 0))
          {
            load_immediate(ARM_LR, src_off, NULL, false);
            ot_check(th_ldr_reg(ARM_LR, base_addr_reg, ARM_LR, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          }

          /* Store to stack */
          if (!store_word_to_base(ARM_LR, ARM_SP, dst_off, 0))
          {
            /* Need a different scratch - use R12 if it's not our base */
            int scratch = (base_addr_reg != ARM_R12) ? ARM_R12 : ARM_R0;
            /* Save R0 if we need it as scratch */
            if (scratch == ARM_R0)
            {
              ot_check(th_push(1 << ARM_R0));
              load_immediate(ARM_R0, dst_off, NULL, false);
              ot_check(th_str_reg(ARM_LR, ARM_SP, ARM_R0, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
              ot_check(th_pop(1 << ARM_R0));
            }
            else
            {
              load_immediate(scratch, dst_off, NULL, false);
              ot_check(th_str_reg(ARM_LR, ARM_SP, scratch, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
            }
          }
        }
      }
      else if (is_64bit)
      {
        /* Store 64-bit value to stack.
         * Need to store both low and high words (little-endian: low at lower address). */
        int lo_offset = stack_offset;
        int hi_offset = stack_offset + 4;

        if (arg->r & VT_LVAL)
        {
          /* Value is in memory, load both words and store to stack */
          SValue sv_copy = *arg;
          load_to_reg(ARM_R12, ARM_LR, &sv_copy);
          /* R12 = low word, LR = high word */
          if (!store_word_to_base(ARM_R12, ARM_SP, lo_offset, 0))
          {
            ot_check(th_push(1 << ARM_R0));
            load_immediate(ARM_R0, lo_offset, NULL, false);
            ot_check(th_str_reg(ARM_R12, ARM_SP, ARM_R0, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
            ot_check(th_pop(1 << ARM_R0));
          }
          if (!store_word_to_base(ARM_LR, ARM_SP, hi_offset, 0))
          {
            ot_check(th_push(1 << ARM_R0));
            load_immediate(ARM_R0, hi_offset, NULL, false);
            ot_check(th_str_reg(ARM_LR, ARM_SP, ARM_R0, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
            ot_check(th_pop(1 << ARM_R0));
          }
        }
        else if (arg->pr0 != PREG_NONE && arg->pr1 != PREG_NONE)
        {
          /* Value is in register pair pr0 (low) and pr1 (high) */
          int lo_reg = arg->pr0;
          int hi_reg = arg->pr1;

          if (!store_word_to_base(lo_reg, ARM_SP, lo_offset, 0))
          {
            load_immediate(ARM_R12, lo_offset, NULL, false);
            ot_check(th_str_reg(lo_reg, ARM_SP, ARM_R12, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          }
          if (!store_word_to_base(hi_reg, ARM_SP, hi_offset, 0))
          {
            load_immediate(ARM_R12, hi_offset, NULL, false);
            ot_check(th_str_reg(hi_reg, ARM_SP, ARM_R12, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          }
        }
        else if ((arg->r & VT_VALMASK) == VT_CONST)
        {
          /* 64-bit constant */
          uint32_t lo_val = (uint32_t)arg->c.i;
          uint32_t hi_val = (uint32_t)(arg->c.i >> 32);

          load_immediate(ARM_R12, lo_val, NULL, false);
          if (!store_word_to_base(ARM_R12, ARM_SP, lo_offset, 0))
          {
            load_immediate(ARM_LR, lo_offset, NULL, false);
            ot_check(th_str_reg(ARM_R12, ARM_SP, ARM_LR, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          }

          load_immediate(ARM_R12, hi_val, NULL, false);
          if (!store_word_to_base(ARM_R12, ARM_SP, hi_offset, 0))
          {
            load_immediate(ARM_LR, hi_offset, NULL, false);
            ot_check(th_str_reg(ARM_R12, ARM_SP, ARM_LR, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          }
        }
        else
        {
          /* Fallback: use load_to_reg to get the 64-bit value into R12+LR, then store */
          SValue sv_copy = *arg;
          load_to_reg(ARM_R12, ARM_LR, &sv_copy);
          if (!store_word_to_base(ARM_R12, ARM_SP, lo_offset, 0))
          {
            ot_check(th_push(1 << ARM_R0));
            load_immediate(ARM_R0, lo_offset, NULL, false);
            ot_check(th_str_reg(ARM_R12, ARM_SP, ARM_R0, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
            ot_check(th_pop(1 << ARM_R0));
          }
          if (!store_word_to_base(ARM_LR, ARM_SP, hi_offset, 0))
          {
            ot_check(th_push(1 << ARM_R0));
            load_immediate(ARM_R0, hi_offset, NULL, false);
            ot_check(th_str_reg(ARM_LR, ARM_SP, ARM_R0, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
            ot_check(th_pop(1 << ARM_R0));
          }
        }
      }
      else
      {
        /* Store 32-bit value to stack */
        if (arg->pr0 != PREG_NONE && !(arg->pr0 & PREG_SPILLED))
        {
          /* Skip if already handled in the pre-shuffle save (R0-R3 sources) */
          if (arg->pr0 <= ARM_R3)
            continue;
          /* Value in register */
          int src_reg = arg->pr0;
          if (arg->r & VT_LVAL)
          {
            /* Register holds a pointer that needs dereferencing.
             * Load the value from the address in the register into R12. */
            ot_check(th_ldr_imm(ARM_R12, src_reg, 0, 6, ENFORCE_ENCODING_NONE));
            src_reg = ARM_R12;
          }
          /* Store the value to stack */
          if (!store_word_to_base(src_reg, ARM_SP, stack_offset, 0))
          {
            /* Offset too large, use scratch register */
            load_immediate(ARM_R12, stack_offset, NULL, false);
            ot_check(th_str_reg(src_reg, ARM_SP, ARM_R12, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          }
        }
        else if ((arg->r & VT_VALMASK) == VT_CONST)
        {
          /* Load constant or symbol address into R12 and store */
          uint32_t imm = (uint32_t)arg->c.i;
          Sym *sym = (arg->r & VT_SYM) ? arg->sym : NULL;
          load_immediate(ARM_R12, imm, sym, false);
          /* If VT_LVAL is set, we need to dereference to get the actual value */
          if (arg->r & VT_LVAL)
          {
            ot_check(th_ldr_imm(ARM_R12, ARM_R12, 0, 6, ENFORCE_ENCODING_NONE));
          }
          if (!store_word_to_base(ARM_R12, ARM_SP, stack_offset, 0))
          {
            load_immediate(ARM_R12, stack_offset, NULL, false);
            ot_check(th_str_reg(ARM_R12, ARM_SP, ARM_R12, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          }
        }
        else if ((arg->r & VT_VALMASK) == VT_LOCAL)
        {
          /* Local variable - compute address (fp + offset) then load/store */
          int local_off = (int)arg->c.i;
          /* Stack parameters live above the saved-register area.
           * When computing their address, fold in offset_to_args (prologue push size). */
          if (arg->r & VT_PARAM)
            local_off += offset_to_args;
          int local_sign = (local_off < 0);
          int local_abs = local_sign ? -local_off : local_off;
          if (arg->r & VT_LVAL)
          {
            /* Load value from local variable into R12 */
            if (!load_word_from_base(ARM_R12, ARM_R7, local_abs, local_sign))
            {
              load_immediate(ARM_R12, local_off, NULL, false);
              ot_check(th_ldr_reg(ARM_R12, ARM_R7, ARM_R12, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
            }
          }
          else
          {
            /* Address of local variable - compute fp + offset */
            if (!ot(th_add_imm(ARM_R12, ARM_R7, local_off, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)))
            {
              load_immediate(ARM_R12, local_off, NULL, false);
              ot_check(th_add_reg(ARM_R12, ARM_R7, ARM_R12, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                  ENFORCE_ENCODING_NONE));
            }
          }
          /* Store R12 to stack */
          if (!store_word_to_base(ARM_R12, ARM_SP, stack_offset, 0))
          {
            /* Need a different scratch for offset since R12 holds the value */
            load_immediate(ARM_LR, stack_offset, NULL, false);
            ot_check(th_str_reg(ARM_R12, ARM_SP, ARM_LR, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          }
        }
        else if ((arg->r & VT_VALMASK) == VT_LLOCAL)
        {
          /* VT_LLOCAL with VT_LVAL: spilled pointer that needs double dereference.
           * The spill slot at FP+offset contains a POINTER, and we need to
           * load the value at that pointer address.
           * Step 1: Load the pointer from spill slot into R12
           * Step 2: Dereference R12 to get the actual value */
          int local_off = (int)arg->c.i;
          if (arg->r & VT_PARAM)
            local_off += offset_to_args;
          int local_sign = (local_off < 0);
          int local_abs = local_sign ? -local_off : local_off;

          /* Step 1: Load the pointer from the spill slot */
          if (!load_word_from_base(ARM_R12, ARM_R7, local_abs, local_sign))
          {
            load_immediate(ARM_R12, local_off, NULL, false);
            ot_check(th_ldr_reg(ARM_R12, ARM_R7, ARM_R12, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          }

          /* Step 2: Dereference the pointer to get the actual value */
          if (arg->r & VT_LVAL)
          {
            ot_check(th_ldr_imm(ARM_R12, ARM_R12, 0, 6, ENFORCE_ENCODING_NONE));
          }

          /* Store R12 to stack */
          if (!store_word_to_base(ARM_R12, ARM_SP, stack_offset, 0))
          {
            load_immediate(ARM_LR, stack_offset, NULL, false);
            ot_check(th_str_reg(ARM_R12, ARM_SP, ARM_LR, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          }
        }
        else
        {
          /* Fallback: use load() for other cases */
          SValue tmp_sv = *arg;
          load(ARM_R12, &tmp_sv);
          if (!store_word_to_base(ARM_R12, ARM_SP, stack_offset, 0))
          {
            load_immediate(ARM_LR, stack_offset, NULL, false);
            ot_check(th_str_reg(ARM_R12, ARM_SP, ARM_LR, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          }
        }
      }
    }
  }

  /* Step 5: Emit the call instruction */
  gcall_or_jump(0, &q->src1);

  /* Restore scratch register exclusion now that call is emitted */
  scratch_global_exclude = saved_scratch_exclude;

  /* Step 6: Clean up stack arguments */
  if (stack_size > 0)
  {
    gadd_sp(stack_size);
    call_site->used_stack_size -= stack_size;
  }

  /* Step 7: Restore argument registers if we pushed them */
  if (arg_regs_in_use != 0)
  {
    uint16_t pop_mask = (uint16_t)arg_regs_in_use;
    ot_check(th_pop(pop_mask));
    call_site->used_stack_size -= __builtin_popcount(arg_regs_in_use) * 4;
  }

  /* Step 8: Handle return value if needed */
  if (!drop_result && q->op == TCCIR_OP_FUNCCALLVAL)
  {
    /* Move return value from R0 (and R1 for 64-bit) to destination */
    if (q->dest.pr0 != PREG_NONE && q->dest.pr0 != ARM_R0)
    {
      ot_check(th_mov_reg(q->dest.pr0, ARM_R0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE, false));
    }

    if (tcc_is_64bit_type(q->dest.type.t) && q->dest.pr1 != PREG_NONE)
    {
      if (q->dest.pr1 != ARM_R1)
      {
        ot_check(th_mov_reg(q->dest.pr1, ARM_R1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE, false));
      }
    }
  }

  /* Clear argument register usage from call site */
  call_site->registers_map &= ~0x0F; /* Clear R0-R3 */

  /* Clean up */
  if (args)
    tcc_free(args);
  if (layout.locs)
    tcc_free(layout.locs);
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
    const char *ctx = "tcc_gen_machine_vla_op";
    int align = (int)q->src2.c.i;
    if (align < 8)
      align = 8;
    if (align & (align - 1))
      tcc_error("alignment is not a power of 2: %i", align);

    /* Compute new SP in-place in the size register (the size value is dead after this op). */
    int r = q->src1.pr0;

    if (r != PREG_NONE)
      thumb_require_materialized_reg(ctx, "size", r);

    /* Fallback for non-IR callers: if src1 wasn't allocated to a register (e.g. constant), load to IP. */
    if (r == PREG_NONE || (q->src1.r & VT_VALMASK) == VT_CONST)
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

ST_FUNC void tcc_gen_machine_func_parameter_op(TACQuadruple *q)
{
  if (q == NULL)
    tcc_error("compiler_error: func_parameter_op requires q");

  /* Decode call_id and parameter index from src2.c.i */
  int call_id = TCCIR_DECODE_CALL_ID(q->src2.c.i);
  int param_index = TCCIR_DECODE_PARAM_IDX(q->src2.c.i);

  /* Find or create call site for this call_id */
  ThumbGenCallSite *call_site = thumb_get_or_create_call_site(call_id);
  if (call_site == NULL)
  {
    tcc_error("compiler_error: failed to allocate call site for call_id=%d", call_id);
    return;
  }

  /* FUNCPARAMVOID is a marker for a 0-argument call.
   * Ensure the call site exists, but do not create a fake argument entry. */
  if (q->op == TCCIR_OP_FUNCPARAMVOID)
    return;

  /* Expand argument list if needed */
  if (param_index >= call_site->function_argument_count)
  {
    int new_count = param_index + 1;
    call_site->function_argument_list = (int *)tcc_realloc(call_site->function_argument_list, new_count * sizeof(int));
    /* Initialize new slots */
    for (int i = call_site->function_argument_count; i < new_count; i++)
    {
      call_site->function_argument_list[i] = -1;
    }
    call_site->function_argument_count = new_count;
  }

  /* Store parameter information - for now just mark as present */
  call_site->function_argument_list[param_index] = 1; /* Mark parameter as present */
}
