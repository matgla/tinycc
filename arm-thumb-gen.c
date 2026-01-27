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

#include <stdlib.h>

/* Target ABI hook: AAPCS-like argument assignment for ARM (R0-R3 + stack).
 *
 * This is a pure layout function: it does not materialize values and does not
 * touch SP. IR can use it to lower calls into explicit CALLSEQ/CALLARG ops.
 */
ST_FUNC int tcc_gen_machine_abi_assign_call_args(const TCCAbiArgDesc *args, int argc, TCCAbiCallLayout *out_layout)
{
  if (!out_layout || (argc > 0 && (!args || !out_layout->locs)))
    return -1;

  /* Initialize layout state for ABI classification */
  TCCAbiCallLayout call_layout;
  memset(&call_layout, 0, sizeof(call_layout));
  call_layout.locs = out_layout->locs;
  call_layout.capacity = out_layout->capacity;
  call_layout.next_reg = 0;       /* ARM AAPCS: start with R0 */
  call_layout.next_stack_off = 0; /* start at stack base */
  call_layout.stack_align = 8;    /* ARM requires 8-byte SP alignment */

  for (int i = 0; i < argc; ++i)
  {
    const TCCAbiArgDesc *ad = &args[i];
    out_layout->locs[i] = tcc_abi_classify_argument(&call_layout, i, ad);
  }

  /* Copy computed layout info from temporary layout to output */
  out_layout->stack_size = call_layout.stack_size;
  out_layout->argc = argc;
  out_layout->stack_align = call_layout.stack_align;
  return 0;
}

#include "arch/fpu/arm/fpv5-sp-d16.h"
#include "arm-thumb-opcodes.h"

#include <inttypes.h>

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
void load_to_dest_ir(IROperand dest, IROperand src);
void load_to_dest(SValue *dest, SValue *src);
static void load_to_reg_ir(int r, int r1, IROperand src);
static void store_ex(int r, SValue *sv, uint32_t extra_exclude);
static void store_ex_ir(int r, IROperand sv, uint32_t extra_exclude);

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
static void load_to_register_ir(int reg, int reg_from, IROperand src);
static void thumb_require_materialized_reg(const char *ctx, const char *operand, int reg);
static void thumb_ensure_not_spilled(const char *ctx, const char *operand, int reg);
static bool thumb_is_hw_reg(int reg);
static int get_struct_base_addr(const IROperand *arg, int default_reg);
int th_has_immediate_value(int r);
int load_word_from_base(int ir, int base, int fc, int sign);
int th_patch_call(int t, int a);
/* Structure to track scratch register allocation with potential save/restore */
typedef struct ScratchRegAlloc
{
  int reg : 31;       /* The allocated scratch register */
  uint32_t saved : 1; /* Whether the register was saved to stack */
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
      /* Never use SP or PC as scratch registers. */
      if (reg == R_SP || reg == R_PC)
        goto no_free_reg;
#ifdef ARM_THUMB_DEBUG_SCRATCH
      fprintf(stderr, "[SCRATCH] -> returning reg=%d (free) exclude=0x%x\n", reg, exclude_regs);
#endif
      result.reg = reg;
      result.saved = 0;
      /* Update global exclude so subsequent calls won't return the same register.
       * This prevents nested scratch allocations from silently reusing and
       * clobbering a still-live operand (e.g. during constant materialization). */
      scratch_global_exclude |= (1u << reg);
      return result;
    }
  }

    no_free_reg:

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
  if (alloc->reg >= 0 && alloc->reg < 32)
  {
    scratch_global_exclude &= ~(1u << alloc->reg);
  }
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
  int pool_size = 0;
  for (int i = 0; i < thumb_gen_state.literal_pool_count; i++)
  {
    if (thumb_gen_state.literal_pool[i].shared_index == -1)
    {
      int entry_size = (thumb_gen_state.literal_pool[i].data_size == 8) ? 8 : 4;
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
   * Use the actual emitted size (ind - branch_pos) to avoid any drift between
   * the precomputed pool size and what was really written.
   * Offset is relative to PC (branch_pos + 4).
   */
  if (thumb_gen_state.generating_function)
  {
    const int branch_after_pool = ind - branch_pos - 4;
    // th_patch_call(branch_pos, branch_after_pool);
    thumb_opcode branch = th_b_t4(branch_after_pool);
    uint16_t *branch_patch = (uint16_t *)(cur_text_section->data + branch_pos);
    branch_patch[0] = (branch.opcode >> 16) & 0xffff;
    branch_patch[1] = branch.opcode & 0xffff;

    if (tcc_state && tcc_state->verbose)
    {
      tcc_warning("literal_pool[%d]: branch_pos=0x%x need_align=%d pool_size=%d ind_end=0x%x branch_after_pool=%d",
                  this_pool, branch_pos, need_align, pool_size, ind, branch_after_pool);
    }
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
static void gcall_or_jump_ir(int is_jmp, IROperand dest);

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

void ggoto(void)
{
  TRACE("'ggoto'");
  {
    SValue target = *vtop;
    tcc_ir_put(tcc_state->ir, TCCIR_OP_IJUMP, &target, NULL, NULL);
  }
  vtop--;
  print_vstack("ggoto");
}

ST_FUNC void tcc_gen_machine_indirect_jump_op(IROperand src1)
{
  /* Indirect jump: target address in src1 register.
   * If VT_LVAL is set, src1.pr0 holds a pointer to the target address,
   * and we need to load the actual target address before jumping. */
  if (src1.pr0_reg == PREG_REG_NONE)
  {
    tcc_error("internal error: IJUMP target not in a register");
  }

  int target_reg = src1.pr0_reg;
  ScratchRegAlloc scratch = {0};

  /* Check if we need to dereference: VT_LVAL means the register holds a pointer
   * to the target address, not the target address itself */
  const int is_address_of = (src1.is_llocal || src1.is_local) && !(src1.is_lval);
  const int needs_deref = (src1.is_lval) && !is_address_of;

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

  IROperand slot = irop_make_none();
  slot.btype = IROP_BTYPE_INT32;
  slot.is_local = 1;
  slot.is_lval = 1;
  slot.u.imm32 = addr;
  slot.vr = -1;

  ot_check(th_mov_reg(R_IP, R_SP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
  store_ex_ir(R_IP, slot, 0);
}

ST_FUNC void gen_vla_sp_restore(int addr)
{
  if (nocode_wanted)
    return;

  IROperand slot = irop_make_none();
  slot.btype = IROP_BTYPE_INT32;
  slot.is_local = 1;
  slot.is_lval = 1;
  slot.u.imm32 = addr;

  load_to_reg_ir(R_IP, 0, slot);
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
  if (dest_reg == PREG_REG_NONE)
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
  if (src_reg == PREG_REG_NONE)
    tcc_error("compiler_error: store_spill_slot requires a source register");

  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
  const int sign = (frame_offset < 0);
  const int abs_offset = sign ? -frame_offset : frame_offset;

  if (!store_word_to_base(src_reg, base_reg, abs_offset, sign))
  {
    /* Avoid clobbering the other half of a 64-bit value when storing
     * paired registers. The allocator uses adjacent register pairs for
     * 64-bit values (e.g. r0/r1, r2/r3, r4/r5). When storing one half,
     * do not use the adjacent register as the scratch offset register.
     */
    uint32_t extra_exclude = 0;
    if (src_reg >= ARM_R0 && src_reg <= ARM_R12)
    {
      int adj = (src_reg & 1) ? (src_reg - 1) : (src_reg + 1);
      if (adj >= ARM_R0 && adj <= ARM_R12 && adj != ARM_SP && adj != ARM_PC)
        extra_exclude |= (1u << adj);
    }

    ScratchRegAlloc rr_alloc =
        th_offset_to_reg_ex(abs_offset, sign, (1u << src_reg) | (1u << base_reg) | extra_exclude);
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

static void th_store32_imm_or_reg_ex(int src_reg, uint32_t base_reg, int abs_off, int sign, uint32_t extra_exclude)
{
  if (!ot(th_str_imm(src_reg, base_reg, abs_off, sign ? 4 : 6, ENFORCE_ENCODING_NONE)))
  {
    ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(abs_off, sign, (1u << src_reg) | (1u << base_reg) | extra_exclude);
    int rr = rr_alloc.reg;
    ot_check(th_str_reg(src_reg, base_reg, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&rr_alloc);
  }
}

static void th_store32_imm_or_reg(int src_reg, uint32_t base_reg, int abs_off, int sign)
{
  th_store32_imm_or_reg_ex(src_reg, base_reg, abs_off, sign, 0);
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

static uint32_t th_store_resolve_base_ir(int src_reg, IROperand sv, int btype, int *abs_off, int *sign,
                                         ScratchRegAlloc *base_alloc, int *has_base_alloc)
{
  int tag = irop_get_tag(sv);
  int32_t off = 0;

  /* Get offset from IROperand */
  if (tag == IROP_TAG_STACKOFF || tag == IROP_TAG_IMM32)
    off = sv.u.imm32;

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

  /* Check if lvalue address is already in a register (VREG with is_lval) */
  if (sv.is_lval && tag == IROP_TAG_VREG && sv.pr0_reg != PREG_REG_NONE)
  {
    base_reg = sv.pr0_reg;
    thumb_require_materialized_reg("store", "address base", base_reg);
    *abs_off = 0;
    *sign = 0;
    return base_reg;
  }

  /* Global symbol lvalue: load the base address into a scratch reg */
  if (sv.is_lval && tag == IROP_TAG_SYMREF)
  {
    IRPoolSymref *symref = irop_get_symref_ex(tcc_state->ir, sv);
    Sym *sym = symref ? symref->sym : NULL;
    Sym *validated_sym = sym ? validate_sym_for_reloc(sym) : NULL;
    int32_t addend = symref ? symref->addend : 0;

    uint32_t exclude_regs = (1u << src_reg);
    *base_alloc = get_scratch_reg_with_save(exclude_regs);
    base_reg = base_alloc->reg;
    *has_base_alloc = 1;

    tcc_machine_load_constant(base_reg, PREG_REG_NONE, addend, 0, validated_sym);
    return base_reg;
  }

  /* Default: stack/local address (FP-based) for STACKOFF */
  return base_reg;
}

void store(int r, SValue *sv)
{
  store_ex(r, sv, 0);
}

/* IROperand-based store functions */
static void store_ex_ir(int r, IROperand sv, uint32_t extra_exclude)
{
  int btype;
  TRACE("'store_ir' reg: %d", r);

  /* IR owns spills: backend store must never be asked to store from a spilled
   * sentinel or a non-hardware register.
   *
   * For hard-float, `r` may be a VFP register (TREG_F0..TREG_F7). Otherwise it
   * must be an integer HW register.
   */
  if (r == PREG_NONE)
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

  btype = irop_get_btype(sv);
  const bool is_64bit = irop_is_64bit(sv);
  const bool is_float_type = (btype == IROP_BTYPE_FLOAT32 || btype == IROP_BTYPE_FLOAT64);

  /* Handle register-to-register store (destination is a physical register, not memory).
   * This happens when storing to a parameter that lives in a callee-saved register. */
  if (!sv.is_lval && !sv.is_local && sv.pr0_reg != PREG_REG_NONE && thumb_is_hw_reg(sv.pr0_reg))
  {
    int dest_reg = sv.pr0_reg;
    thumb_require_materialized_reg("store", "dest", dest_reg);
    if (dest_reg != r)
    {
      ot_check(
          th_mov_reg(dest_reg, r, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
    }
    /* For 64-bit types, also move the high word */
    if (is_64bit && sv.pr1_reg != PREG_REG_NONE)
    {
      /* The caller should set sv.pr1 to the destination high register.
       * Source high is assumed to be the next register (r+1) for 64-bit values. */
      int dest_hi = sv.pr1_reg;
      if (dest_hi != dest_reg)
      {
        int src_hi = r + 1;
        if (!thumb_is_hw_reg(src_hi) || src_hi == R_SP || src_hi == R_PC)
          tcc_error("compiler_error: cannot store 64-bit reg pair - invalid source high register %d", src_hi);
        thumb_require_materialized_reg("store", "dest.high", dest_hi);
        if (dest_hi != src_hi)
        {
          ot_check(th_mov_reg(dest_hi, src_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE, false));
        }
      }
    }
    return;
  }

  if (sv.is_lval || sv.is_local)
  {
    int abs_off, sign;
    ScratchRegAlloc base_alloc = (ScratchRegAlloc){0};
    int has_base_alloc = 0;
    uint32_t base = th_store_resolve_base_ir(r, sv, btype, &abs_off, &sign, &base_alloc, &has_base_alloc);

    /* Check if source is VFP or integer register.
     * Only use VFP instructions if hard float ABI is enabled.
     */
    if (is_float_type)
    {
      if (tcc_state->float_abi == ARM_HARD_FLOAT && r >= TREG_F0 && r <= TREG_F7)
      {
        /* VFP source - use VSTR */
        if (btype != IROP_BTYPE_FLOAT32)
          ot_check(th_vstr(base, r, !sign, 1, abs_off));
        else
          ot_check(th_vstr(base, r, !sign, 0, abs_off));
      }
      else
      {
        /* Soft-float (or integer-reg float values): use integer stores. */
        if (btype == IROP_BTYPE_FLOAT32)
        {
          th_store32_imm_or_reg_ex(r, base, abs_off, sign, extra_exclude);
        }
        else
        {
          /* Double precision - two 32-bit stores (low word first).
           * IR owns spills: the caller must provide an explicit high-word
           * register in sv.pr1; do not guess r+1.
           */
          int r_high = sv.pr1_reg;
          if (r_high == PREG_NONE)
          {
            /* Legacy (non-IR) backend paths may still call store() with only
             * the low register. In that case, assume a conventional register
             * pair (low=r, high=r+1). */
            if (thumb_is_hw_reg(r) && thumb_is_hw_reg(r + 1) && (r + 1) != R_SP && (r + 1) != R_PC)
              r_high = r + 1;
            else
              tcc_error("compiler_error: cannot store double - missing source high register (sv.pr1_reg)");
          }
          thumb_require_materialized_reg("store", "src.high", r_high);
          if (r_high == R_SP || r_high == R_PC)
            tcc_error("compiler_error: cannot store double - invalid source high register %d", r_high);

          /* High word is at +4 from low word. When sign=1 (negative offset),
           * we need to decrease abs_off to get a higher address. */
          int hi_abs_off = sign ? (abs_off - 4) : (abs_off + 4);
          /* When storing the low word, exclude r_high from scratch allocation
           * to prevent clobbering the high word value before it's stored. */
          th_store32_imm_or_reg_ex(r, base, abs_off, sign, (1u << r_high));
          th_store32_imm_or_reg(r_high, base, hi_abs_off, sign);
        }
      }
    }
    else if (btype == IROP_BTYPE_INT16)
    {
      /* 16-bit short store */
      th_store16_imm_or_reg(r, base, abs_off, sign);
    }
    else if (btype == IROP_BTYPE_INT8)
    {
      /* 8-bit byte store */
      th_store8_imm_or_reg(r, base, abs_off, sign);
    }
    else if (is_64bit)
    {
      /* Long long / 64-bit int - store both low and high words */
      int r_high = sv.pr1_reg;
      if (r_high == PREG_NONE)
      {
        /* Legacy (non-IR) backend paths may still call store() with only the
         * low register. Assume the value is in a register pair (r, r+1). */
        if (thumb_is_hw_reg(r) && thumb_is_hw_reg(r + 1) && (r + 1) != R_SP && (r + 1) != R_PC)
          r_high = r + 1;
        else
          tcc_error("compiler_error: cannot store llong - missing source high register (sv.pr1_reg)");
      }
      thumb_require_materialized_reg("store", "src.high", r_high);
      if (r_high == R_SP || r_high == R_PC)
        tcc_error("compiler_error: cannot store llong - invalid source high register %d", r_high);

      /* High word is at +4 from low word. When sign=1 (negative offset),
       * we need to decrease abs_off to get a higher address. */
      int hi_abs_off = sign ? (abs_off - 4) : (abs_off + 4);
      /* When storing the low word, exclude r_high from scratch allocation
       * to prevent clobbering the high word value before it's stored. */
      th_store32_imm_or_reg_ex(r, base, abs_off, sign, (1u << r_high));
      th_store32_imm_or_reg(r_high, base, hi_abs_off, sign);
    }
    else
    {
      /* Default 32-bit store */
      TRACE("store: sign: %x, r: %x, base: %x, off: %x", sign, r, base, abs_off);
      th_store32_imm_or_reg_ex(r, base, abs_off, sign, extra_exclude);
      TRACE("done");
    }

    if (has_base_alloc)
      restore_scratch_reg(&base_alloc);
  }
}

void store_ir(int r, IROperand sv)
{
  store_ex_ir(r, sv, 0);
}

static void store_ex(int r, SValue *sv, uint32_t extra_exclude)
{
  /* Legacy wrapper: convert SValue to IROperand and delegate to store_ex_ir */
  const IROperand sv_ir = svalue_to_iroperand(tcc_state->ir, sv);
  store_ex_ir(r, sv_ir, extra_exclude);
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
  if (dest_reg == PREG_REG_NONE)
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
  if (dest_reg == PREG_REG_NONE)
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
  if (dest_reg == PREG_REG_NONE)
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
  if (dest_reg == PREG_REG_NONE)
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

/* Load value from memory at base+offset into register(s).
 * Uses IROP_BTYPE_* constants directly, no VT_* conversion needed.
 */
static void load_from_base_ir(int r, int r1, int irop_btype, int is_unsigned, int fc, int sign, uint32_t base)
{
  int success = 0;
  const int is_64bit = (irop_btype == IROP_BTYPE_INT64 || irop_btype == IROP_BTYPE_FLOAT64);

  TRACE("load_from_base_ir: r=%d, r1=%d, irop_btype=%d, is_unsigned=%d, fc=%d, sign=%d, base=%d", r, r1, irop_btype,
        is_unsigned, fc, sign, base);

  if (is_64bit)
  {
    /* 64-bit value (double float or long long) - load to register pair */
    int ir_high = r1;
    if (ir_high < 0 || ir_high == PREG_REG_NONE)
    {
      ir_high = r + 1;
      if (ir_high == R_SP || ir_high == R_PC)
      {
        tcc_error("compiler_error: cannot load 64-bit value - no valid high register");
      }
    }

    /* If base overlaps with destination, preserve it */
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

    /* Load low word */
    success = load_word_from_base(r, base_reg, fc, sign);
    if (!success)
    {
      ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(fc, sign, (1u << r) | (1u << base_reg) | (1u << ir_high));
      int rr = rr_alloc.reg;
      ot_check(th_ldr_reg(r, base_reg, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      restore_scratch_reg(&rr_alloc);
    }

    /* Load high word */
    int fc_high = sign ? (fc - 4) : (fc + 4);
    success = load_word_from_base(ir_high, base_reg, fc_high, sign);
    if (!success)
    {
      ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(fc_high, sign, (1u << r) | (1u << base_reg) | (1u << ir_high));
      int rr = rr_alloc.reg;
      ot_check(th_ldr_reg(ir_high, base_reg, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      restore_scratch_reg(&rr_alloc);
    }

    if (base_alloc.saved)
      restore_scratch_reg(&base_alloc);
    return;
  }

  if (irop_btype == IROP_BTYPE_INT16)
  {
    if (!is_unsigned)
      success = load_short_from_base(r, base, fc, sign);
    else
      success = load_ushort_from_base(r, base, fc, sign);
  }
  else if (irop_btype == IROP_BTYPE_INT8)
  {
    if (!is_unsigned)
      success = load_byte_from_base(r, base, fc, sign);
    else
      success = load_ubyte_from_base(r, base, fc, sign);
  }
  else
  {
    /* IROP_BTYPE_INT32, IROP_BTYPE_FLOAT32, IROP_BTYPE_STRUCT, IROP_BTYPE_FUNC: load as word */
    success = load_word_from_base(r, base, fc, sign);
  }

  if (!success)
  {
    ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(fc, sign, (1u << r) | (1u << base));
    int rr = rr_alloc.reg;
    if (irop_btype == IROP_BTYPE_INT16)
    {
      if (is_unsigned)
        ot_check(th_ldrh_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      else
        ot_check(th_ldrsh_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    else if (irop_btype == IROP_BTYPE_INT8)
    {
      if (is_unsigned)
        ot_check(th_ldrb_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      else
        ot_check(th_ldrsb_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    else
      ot_check(th_ldr_reg(r, base, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&rr_alloc);
  }
}

void load_to_dest(SValue *dest, SValue *src)
{
  const IROperand d = svalue_to_iroperand(tcc_state->ir, dest);
  const IROperand s = svalue_to_iroperand(tcc_state->ir, src);
  load_to_dest_ir(d, s);
}

void load_to_dest_ir(IROperand dest, IROperand src)
{
  const char *ctx = "load_to_dest_ir";
  int tag = irop_get_tag(src);
  int btype = irop_get_btype(src);

  /* If we're about to write into the register currently used to cache a global
   * symbol base address, invalidate the cache first. Otherwise the cache can
   * become stale (same register, different contents) and later loads may
   * incorrectly reuse it (e.g. clobbering stdout setup when loading a literal). */
  uint8_t dest_pr0_packed = (dest.pr0_spilled ? PREG_SPILLED : 0) | dest.pr0_reg;
  uint8_t dest_pr1_packed = (dest.pr1_spilled ? PREG_SPILLED : 0) | dest.pr1_reg;
  if (thumb_gen_state.cached_global_reg != PREG_NONE &&
      (dest_pr0_packed == thumb_gen_state.cached_global_reg || dest_pr1_packed == thumb_gen_state.cached_global_reg))
  {
    thumb_gen_state.cached_global_sym = NULL;
    thumb_gen_state.cached_global_reg = PREG_NONE;
  }

  /* Check if it's a float type based on btype */
  int is_float_type = (btype == IROP_BTYPE_FLOAT32 || btype == IROP_BTYPE_FLOAT64);
  int is_64bit = irop_is_64bit(src);

  /* Handle based on tag type */
  switch (tag)
  {
  case IROP_TAG_NONE:
    /* Nothing to load */
    return;

  case IROP_TAG_VREG:
  {
    /* Value is in a register (possibly register-indirect if is_lval) */
    int src_reg = src.pr0_reg;
    if (src_reg == PREG_REG_NONE)
    {
      tcc_error("compiler_error: IROP_TAG_VREG with no physical register");
    }

    if (src.is_lval)
    {
      /* Register-indirect load: src_reg holds address */
      thumb_require_materialized_reg(ctx, "lvalue base", src_reg);
      int pr1_for_load = dest.pr1_spilled ? PREG_REG_NONE : dest.pr1_reg;
      load_from_base_ir(dest.pr0_reg, pr1_for_load, btype, src.is_unsigned, 0, 0, src_reg);
      return;
    }

    /* Direct register-to-register move */
    thumb_require_materialized_reg(ctx, "source register", src_reg);

    if (is_float_type)
    {
      /* Check if we're moving between VFP registers or integer registers. */
      if (tcc_state->float_abi == ARM_HARD_FLOAT && dest.pr0_reg >= TREG_F0 && dest.pr0_reg <= TREG_F7 &&
          src_reg >= TREG_F0 && src_reg <= TREG_F7)
      {
        /* VFP to VFP move */
        if (btype == IROP_BTYPE_FLOAT32)
          ot_check(th_vmov_register(dest.pr0_reg, src_reg, 0));
        else
          ot_check(th_vmov_register(dest.pr0_reg, src_reg, 1));
      }
      else
      {
        /* Integer register move (soft float) */
        if (dest.pr0_reg != src_reg)
        {
          ot_check(th_mov_reg(dest.pr0_reg, src_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE, false));
        }
        if (is_64bit && dest.pr1_reg != PREG_REG_NONE)
        {
          int src_high = (src.pr1_reg != PREG_REG_NONE) ? src.pr1_reg : (src_reg + 1);
          if (dest.pr1_reg != src_high)
          {
            ot_check(th_mov_reg(dest.pr1_reg, src_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                ENFORCE_ENCODING_NONE, false));
          }
        }
      }
    }
    else
    {
      /* Non-float register move */
      if (dest.pr0_reg != src_reg)
      {
        ot_check(th_mov_reg(dest.pr0_reg, src_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE, false));
      }
      if (dest.pr1_reg != PREG_REG_NONE && is_64bit)
      {
        if (src.pr1_reg == PREG_REG_NONE)
        {
          tcc_error("compiler_error: source high register missing for 64-bit move\n");
        }
        if (dest.pr1_reg != src.pr1_reg)
        {
          ot_check(th_mov_reg(dest.pr1_reg, src.pr1_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE, false));
        }
      }
    }
    return;
  }

  case IROP_TAG_IMM32:
  {
    /* 32-bit immediate constant */
    int64_t value = src.is_unsigned ? (int64_t)(uint32_t)src.u.imm32 : (int64_t)src.u.imm32;
    int pr1_for_const = dest.pr1_spilled ? PREG_REG_NONE : dest.pr1_reg;
    tcc_machine_load_constant(dest.pr0_reg, pr1_for_const, value, 0, NULL);
    return;
  }

  case IROP_TAG_STACKOFF:
  {
    /* Stack-relative offset (VT_LOCAL or VT_LLOCAL semantics) */
    int frame_offset = src.u.imm32;
    int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;

    /* Apply offset_to_args for stack-passed parameters */
    if (src.is_param && frame_offset >= 0)
    {
      frame_offset += offset_to_args;
    }

    int sign = (frame_offset < 0);
    int abs_offset = sign ? -frame_offset : frame_offset;

    if (src.is_lval)
    {
      /* Load value from stack location */
      int pr1_for_load = dest.pr1_spilled ? PREG_REG_NONE : dest.pr1_reg;
      load_from_base_ir(dest.pr0_reg, pr1_for_load, btype, src.is_unsigned, abs_offset, sign, base_reg);
    }
    else
    {
      /* Address-of stack slot: compute FP/SP + offset */
      tcc_machine_addr_of_stack_slot(dest.pr0_reg, src.u.imm32, src.is_param);
    }
    return;
  }

  case IROP_TAG_F32:
  {
    /* Inline 32-bit float constant */
    union
    {
      uint32_t bits;
      float f;
    } u;
    u.bits = src.u.f32_bits;
    /* Load as 32-bit integer constant (soft float) */
    tcc_machine_load_constant(dest.pr0_reg, PREG_NONE, (int64_t)u.bits, 0, NULL);
    return;
  }
  case IROP_TAG_I64:
  case IROP_TAG_F64:
  {
    const uint64_t value = irop_get_imm64_ex(tcc_state->ir, src);
    if (dest.pr1_spilled)
    {
      /* High register is spilled - this case should be handled at the IR level
       * by first loading to a scratch reg then storing to spill slot.
       * For now, error out to identify where this is happening. */
      tcc_error("compiler_error: load_to_dest_ir I64/F64: dest.pr1 is spilled, need IR-level handling");
    }
    if (dest.pr1_reg == PREG_REG_NONE)
    {
      /* No high register allocated - use a scratch register for high word */
      ScratchRegAlloc hi_alloc = get_scratch_reg_with_save((1u << dest.pr0_reg) | (1u << ARM_SP) | (1u << ARM_PC));
      tcc_machine_load_constant(dest.pr0_reg, hi_alloc.reg, (int64_t)value, 1, NULL);
      /* The high word is loaded but discarded - caller must not need it */
      restore_scratch_reg(&hi_alloc);
    }
    else
    {
      tcc_machine_load_constant(dest.pr0_reg, dest.pr1_reg, (int64_t)value, 1, NULL);
    }
    return;
  }
  case IROP_TAG_SYMREF:
  {
    /* Symbol reference from pool - requires ir state */
    IRPoolSymref *symref = irop_get_symref_ex(tcc_state->ir, src);
    Sym *sym = symref ? symref->sym : NULL;
    int32_t addend = symref ? symref->addend : 0;
    const int pr1_for_const = dest.pr1_spilled ? PREG_REG_NONE : dest.pr1_reg;

    if (src.is_lval)
    {
      /* Load value from global symbol address:
       * 1. Load symbol address into a scratch register
       * 2. Load the value from that address (with addend offset) */
      Sym *validated_sym = sym ? validate_sym_for_reloc(sym) : NULL;
      uint32_t exclude_regs = (1u << dest.pr0_reg);
      if (pr1_for_const != PREG_REG_NONE)
        exclude_regs |= (1u << pr1_for_const);
      ScratchRegAlloc base_alloc = get_scratch_reg_with_save(exclude_regs);
      int base_reg = base_alloc.reg;

      /* Load symbol address into scratch register */
      tcc_machine_load_constant(base_reg, PREG_REG_NONE, 0, 0, validated_sym);

      /* Load value from the address with addend offset */
      int sign = (addend < 0);
      int abs_offset = sign ? -addend : addend;
      load_from_base_ir(dest.pr0_reg, pr1_for_const, btype, src.is_unsigned, abs_offset, sign, base_reg);

      restore_scratch_reg(&base_alloc);
      return;
    }

    /* Not lval: just load the symbol address (with addend baked in by tcc_machine_load_constant) */
    return tcc_machine_load_constant(dest.pr0_reg, pr1_for_const, addend, is_64bit, sym);
  }

  default:
    tcc_error("compiler_error: unknown IROperand tag in load_to_dest_ir: %d\n", tag);
    return;
  }
}

ST_FUNC void tcc_machine_load_to_reg(int r, int r1, SValue *src)
{
  const IROperand s = svalue_to_iroperand(tcc_state->ir, src);
  load_to_reg_ir(r, r1, s);
}

/* Wrapper for loading IROperand to a register pair */
static void load_to_reg_ir(int r, int r1, IROperand src)
{
  IROperand dest = irop_make_none();
  dest.pr0_reg = r;
  dest.pr0_spilled = 0;
  dest.pr1_reg = r1; /* PREG_REG_NONE for 32-bit, actual register for 64-bit */
  dest.pr1_spilled = 0;
  dest.btype = src.btype;
  load_to_dest_ir(dest, src);
}

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
  if (reg == PREG_REG_NONE || !reg_is_hw)
  {
    tcc_error("compiler_error: %s expects %s in a physical register (pr=%d)", ctx, operand, reg);
  }
}

static void thumb_ensure_not_spilled(const char *ctx, const char *operand, int reg)
{
  if (reg != PREG_REG_NONE)
  {
    const bool reg_is_hw = (reg >= 0) && (reg <= 15);
    if (!reg_is_hw)
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

static void thumb_prepare_dest_pair_for_64bit_op_ir(const char *ctx, IROperand *dest, int *rd_low, int *rd_high,
                                                    ScratchRegAlloc *rd_low_alloc, ScratchRegAlloc *rd_high_alloc,
                                                    bool *store_low, bool *store_high, uint32_t *exclude_mask)
{
  if (!dest || !rd_low || !rd_high || !rd_low_alloc || !rd_high_alloc || !store_low || !store_high || !exclude_mask)
    tcc_error("compiler_error: invalid arguments to thumb_prepare_dest_pair_for_64bit_op_ir");

  *rd_low = dest->pr0_reg;
  *rd_high = dest->pr1_reg;
  *store_low = false;
  *store_high = false;

  if (((*rd_high == PREG_REG_NONE) || (*rd_high == *rd_low)) && dest->pr0_reg != PREG_REG_NONE && !dest->is_lval &&
      !dest->is_local && !dest->is_llocal)
  {
    int candidate = *rd_low + 1;
    if (thumb_is_hw_reg(*rd_low) && thumb_is_hw_reg(candidate) && candidate != R_SP && candidate != R_PC)
    {
      dest->pr1_reg = candidate;
      dest->pr1_spilled = 0;
      *rd_high = candidate;
    }
    else
    {
      tcc_error("compiler_error: %s missing high register for 64-bit destination (pr0=%d)", ctx, *rd_low);
    }
  }

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

static void thumb_store_dest_pair_if_needed_ir(IROperand dest, int rd_low, int rd_high, bool store_low, bool store_high)
{
  if (irop_is_none(dest))
    return;

  const bool dest_is_reg = (!dest.is_lval && !dest.is_local && !dest.is_llocal && dest.pr0_reg != PREG_REG_NONE &&
                            thumb_is_hw_reg(dest.pr0_reg));

  if (store_low)
  {
    if (dest_is_reg)
    {
      if (dest.pr0_reg != rd_low)
      {
        ot_check(th_mov_reg(dest.pr0_reg, rd_low, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE, false));
      }
    }
    else
    {
      IROperand dest_lo = dest;
      dest_lo.pr1_reg = PREG_REG_NONE;
      dest_lo.pr1_spilled = 0;
      dest_lo.btype = IROP_BTYPE_INT32;
      store_ex_ir(rd_low, dest_lo, store_high ? (1u << rd_high) : 0);
    }
  }
  if (store_high)
  {
    if (dest_is_reg)
    {
      int dest_high = dest.pr1_reg;
      if (dest_high == PREG_REG_NONE || dest_high == dest.pr0_reg)
      {
        int candidate = dest.pr0_reg + 1;
        if (!dest.pr0_spilled && thumb_is_hw_reg(dest.pr0_reg) && thumb_is_hw_reg(candidate) && candidate != R_SP &&
            candidate != R_PC)
          dest_high = candidate;
      }
      if (dest_high == PREG_REG_NONE)
        tcc_error("compiler_error: missing high register for 64-bit storeback");
      if (dest_high != rd_high)
      {
        ot_check(th_mov_reg(dest_high, rd_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE, false));
      }
    }
    else
    {
      IROperand dest_hi = dest;
      dest_hi.pr1_reg = PREG_REG_NONE;
      dest_hi.pr1_spilled = 0;
      dest_hi.btype = IROP_BTYPE_INT32;
      if (irop_get_tag(dest_hi) == IROP_TAG_SYMREF)
      {
        IRPoolSymref *symref = irop_get_symref_ex(tcc_state->ir, dest_hi);
        if (symref)
        {
          uint32_t idx = tcc_ir_pool_add_symref(tcc_state->ir, symref->sym, symref->addend + 4, symref->flags);
          dest_hi.u.pool_idx = idx;
        }
      }
      else
      {
        dest_hi.u.imm32 += 4;
      }
      store_ir(rd_high, dest_hi);
    }
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

static bool thumb_irop_has_immediate_value(IROperand op)
{
  int tag = irop_get_tag(op);
  return tag == IROP_TAG_IMM32 || tag == IROP_TAG_I64 || tag == IROP_TAG_F32 || tag == IROP_TAG_F64;
}

static bool thumb_irop_needs_value_load(IROperand op)
{
  const bool is_address_of = (op.is_local || op.is_llocal) && !op.is_lval;
  const bool is_sym_address = (op.is_sym || irop_get_tag(op) == IROP_TAG_SYMREF) && !op.is_lval;
  return is_address_of || is_sym_address;
}

static void thumb_materialize_src1_for_64op(const char *ctx, IROperand src1, bool src1_is64, int rd_low, int rd_high,
                                            int *rn_low, int *rn_high, ScratchRegAlloc *rn_low_alloc,
                                            ScratchRegAlloc *rn_high_alloc, uint32_t *exclude)
{
  const bool src1_is_imm = (src1.pr0_reg == PREG_REG_NONE) && thumb_irop_has_immediate_value(src1);
  int low = src1.pr0_reg;
  int high = (src1_is64 ? src1.pr1_reg : PREG_REG_NONE);
  const bool needs_value_load = thumb_irop_needs_value_load(src1);

  if (src1_is_imm)
  {
    Sym *sym = src1.is_sym ? irop_get_sym_ex(tcc_state->ir, src1) : NULL;
    const int64_t imm = irop_get_imm64_ex(tcc_state->ir, src1);
    if (src1_is64)
    {
      tcc_machine_load_constant(rd_low, rd_high, imm, 1, sym);
      low = rd_low;
      high = rd_high;
    }
    else
    {
      tcc_machine_load_constant(rd_low, PREG_NONE, imm, 0, sym);
      low = rd_low;
      high = PREG_REG_NONE;
    }
  }
  else if (!needs_value_load && !src1.is_lval && thumb_is_hw_reg(low) &&
           (!src1_is64 || (high != PREG_REG_NONE && thumb_is_hw_reg(high))))
  {
    thumb_require_materialized_reg(ctx, "src1.low", low);
    if (src1_is64 && high != PREG_REG_NONE)
      thumb_ensure_not_spilled(ctx, "src1.high", high);
    *exclude |= (1u << low);
    if (src1_is64 && high != PREG_REG_NONE)
      *exclude |= (1u << high);
  }
  else
  {
    *rn_low_alloc = get_scratch_reg_with_save(*exclude);
    low = rn_low_alloc->reg;
    *exclude |= (1u << low);
    if (src1_is64)
    {
      *rn_high_alloc = get_scratch_reg_with_save(*exclude);
      high = rn_high_alloc->reg;
      *exclude |= (1u << high);
      IROperand src1_tmp = src1;
      load_to_reg_ir(low, high, src1_tmp);
    }
    else
    {
      high = PREG_REG_NONE;
      IROperand src1_tmp = src1;
      load_to_reg_ir(low, PREG_NONE, src1_tmp);
    }
  }

  *rn_low = low;
  *rn_high = high;
}

static void thumb_materialize_src2_for_64op(const char *ctx, IROperand src2, bool src2_is64, bool src2_is_imm,
                                            int *rm_low, int *rm_high, ScratchRegAlloc *rm_low_alloc,
                                            ScratchRegAlloc *rm_high_alloc, uint32_t *exclude)
{
  if (src2_is_imm)
  {
    *rm_low = PREG_REG_NONE;
    *rm_high = PREG_REG_NONE;
    return;
  }

  int low = src2.pr0_reg;
  int high = (src2_is64 ? src2.pr1_reg : PREG_REG_NONE);
  const bool needs_value_load = thumb_irop_needs_value_load(src2);

  if (!needs_value_load && !src2.is_lval && thumb_is_hw_reg(low) &&
      (!src2_is64 || (high != PREG_REG_NONE && thumb_is_hw_reg(high))))
  {
    thumb_require_materialized_reg(ctx, "src2.low", low);
    if (src2_is64 && high != PREG_REG_NONE)
      thumb_ensure_not_spilled(ctx, "src2.high", high);
  }
  else
  {
    *rm_low_alloc = get_scratch_reg_with_save(*exclude);
    low = rm_low_alloc->reg;
    *exclude |= (1u << low);
    if (src2_is64)
    {
      *rm_high_alloc = get_scratch_reg_with_save(*exclude);
      high = rm_high_alloc->reg;
      *exclude |= (1u << high);
      IROperand src2_tmp = src2;
      load_to_reg_ir(low, high, src2_tmp);
    }
    else
    {
      high = PREG_REG_NONE;
      IROperand src2_tmp = src2;
      load_to_reg_ir(low, PREG_NONE, src2_tmp);
    }
  }

  *rm_low = low;
  *rm_high = high;
}

static void thumb_emit_opcode64_imm_ir(IROperand src1, IROperand src2, IROperand dest, TccIrOp op, const char *ctx,
                                       ThumbDataProcessingHandler regular, ThumbDataProcessingHandler carry)
{
  const bool src2_is_imm = thumb_irop_has_immediate_value(src2);
  const uint64_t src2_imm = (uint64_t)irop_get_imm64_ex(tcc_state->ir, src2);
  const uint32_t imm_low = (uint32_t)(src2_imm & 0xffffffffu);
  const uint32_t imm_high = (uint32_t)(src2_imm >> 32);

  /* dest might not be in physical regs (e.g. lives in memory). */
  uint32_t exclude = 0;
  ScratchRegAlloc rd_low_alloc = {0};
  ScratchRegAlloc rd_high_alloc = {0};
  bool store_low = false;
  bool store_high = false;
  int rd_low = dest.pr0_reg;
  int rd_high = dest.pr1_reg;
  thumb_prepare_dest_pair_for_64bit_op_ir(ctx, &dest, &rd_low, &rd_high, &rd_low_alloc, &rd_high_alloc, &store_low,
                                          &store_high, &exclude);

  const bool src1_is64 = irop_is_64bit(src1);
  const bool src2_is64 = irop_is_64bit(src2);

  /* Materialize src1. */
  int rn_low = src1.pr0_reg;
  int rn_high = (src1_is64 ? src1.pr1_reg : PREG_REG_NONE);
  ScratchRegAlloc rn_low_alloc = {0};
  ScratchRegAlloc rn_high_alloc = {0};
  thumb_materialize_src1_for_64op(ctx, src1, src1_is64, rd_low, rd_high, &rn_low, &rn_high, &rn_low_alloc,
                                  &rn_high_alloc, &exclude);

  /* Materialize src2 (if not immediate). */
  int rm_low = src2.pr0_reg;
  int rm_high = (src2_is64 ? src2.pr1_reg : PREG_REG_NONE);
  ScratchRegAlloc rm_low_alloc = {0};
  ScratchRegAlloc rm_high_alloc = {0};
  thumb_materialize_src2_for_64op(ctx, src2, src2_is64, src2_is_imm, &rm_low, &rm_high, &rm_low_alloc, &rm_high_alloc,
                                  &exclude);

  /* Low word sets carry/flags for the high word. */
  if (src2_is_imm)
    thumb_emit_op_imm_fallback(rd_low, rn_low, imm_low, FLAGS_BEHAVIOUR_SET, regular);
  else
    ot_check(
        regular.reg_handler(rd_low, rn_low, rm_low, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));

  if (src2_is_imm)
  {
    if (rn_high != PREG_REG_NONE)
    {
      ot_check(carry.imm_handler(rd_high, rn_high, imm_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    }
    else
    {
      ot_check(th_mov_imm(rd_high, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      ot_check(carry.imm_handler(rd_high, rd_high, imm_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    }
  }
  else if (rn_high != PREG_REG_NONE && rm_high != PREG_REG_NONE)
  {
    ot_check(carry.reg_handler(rd_high, rn_high, rm_high, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                               ENFORCE_ENCODING_NONE));
  }
  else if (rn_high != PREG_REG_NONE)
  {
    ot_check(carry.imm_handler(rd_high, rn_high, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }
  else if (rm_high != PREG_REG_NONE)
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

  thumb_store_dest_pair_if_needed_ir(dest, rd_low, rd_high, store_low, store_high);
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
  IROperand imm_irop = irop_make_imm32(0, (int32_t)value, IROP_BTYPE_INT32);
  imm_irop.is_unsigned = 1;
  load_to_reg_ir(rd, PREG_NONE, imm_irop);
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

static void thumb_emit_logical64_op(IROperand src1, IROperand src2, IROperand dest, TccIrOp op,
                                    ThumbDataProcessingHandler handler, thumb_u64_fold_t fold64,
                                    thumb_u32_fold_t fold32, const char *ctx)
{
  static int debug_logical64 = -1;
  if (debug_logical64 == -1)
    debug_logical64 = (getenv("TCC_DEBUG_LOGICAL64") != NULL);

  /* Only treat true immediate operands as immediates.
   * Non-immediate values may legitimately have pr0==PREG_NONE (e.g. stack locals)
   * and must be loaded/materialized, not misclassified as constants.
   */
  const bool src1_is_imm = thumb_irop_has_immediate_value(src1);
  const bool src2_is_imm = thumb_irop_has_immediate_value(src2);
  const uint64_t src1_imm = (uint64_t)irop_get_imm64_ex(tcc_state->ir, src1);
  const uint64_t src2_imm = (uint64_t)irop_get_imm64_ex(tcc_state->ir, src2);

  if (src1_is_imm && src2_is_imm)
  {
    /* Constant folding: load the computed result directly to destination */
    int64_t folded_value = (int64_t)fold64(src1_imm, src2_imm);
    uint32_t exclude = 0;
    ScratchRegAlloc rd_low_alloc = {0};
    ScratchRegAlloc rd_high_alloc = {0};
    bool store_low = false;
    bool store_high = false;
    int rd_low = dest.pr0_reg;
    int rd_high = dest.pr1_reg;

    thumb_prepare_dest_pair_for_64bit_op_ir(ctx, &dest, &rd_low, &rd_high, &rd_low_alloc, &rd_high_alloc, &store_low,
                                            &store_high, &exclude);
    tcc_machine_load_constant(rd_low, rd_high, folded_value, irop_is_64bit(dest), NULL);
    thumb_store_dest_pair_if_needed_ir(dest, rd_low, rd_high, store_low, store_high);
    restore_scratch_reg(&rd_high_alloc);
    restore_scratch_reg(&rd_low_alloc);
    return;
  }

  ScratchRegAlloc rd_low_alloc = {0};
  ScratchRegAlloc rd_high_alloc = {0};
  bool store_low = false;
  bool store_high = false;
  int rd_low = dest.pr0_reg;
  int rd_high = dest.pr1_reg;
  uint32_t dest_exclude = 0;

  if (src1_is_imm || src2_is_imm)
  {
    const IROperand reg_src = src1_is_imm ? src2 : src1;
    const uint64_t imm64 = src1_is_imm ? src1_imm : src2_imm;
    const uint32_t imm_low = (uint32_t)(imm64 & 0xffffffffu);
    const uint32_t imm_high = (uint32_t)(imm64 >> 32);
    const bool reg_src_is64 = irop_is_64bit(reg_src);
    ScratchRegAlloc reg_src_lo_alloc = (ScratchRegAlloc){0};
    ScratchRegAlloc reg_src_hi_alloc = (ScratchRegAlloc){0};
    int rn_low = reg_src.pr0_reg;
    int rn_high = (reg_src_is64 ? reg_src.pr1_reg : PREG_REG_NONE);

    thumb_prepare_dest_pair_for_64bit_op_ir(ctx, &dest, &rd_low, &rd_high, &rd_low_alloc, &rd_high_alloc, &store_low,
                                            &store_high, &dest_exclude);

    thumb_materialize_src1_for_64op(ctx, reg_src, reg_src_is64, rd_low, rd_high, &rn_low, &rn_high, &reg_src_lo_alloc,
                                    &reg_src_hi_alloc, &dest_exclude);

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

    if (rn_high == PREG_REG_NONE)
    {
      const uint32_t folded_high = fold32(0u, imm_high);
      thumb_materialize_u32(rd_high, folded_high);
    }
    else
    {
      thumb_emit_dp_imm_with_fallback(handler, rd_high, rn_high, imm_high, imm_exclude);
    }

    if (reg_src_hi_alloc.reg != 0)
      restore_scratch_reg(&reg_src_hi_alloc);
    if (reg_src_lo_alloc.reg != 0)
      restore_scratch_reg(&reg_src_lo_alloc);

    goto thumb_logical64_cleanup;
  }

  const bool src1_is64 = irop_is_64bit(src1);
  const bool src2_is64 = irop_is_64bit(src2);

  int src1_lo = src1.pr0_reg;
  int src1_hi = src1.pr1_reg;
  int src2_lo = src2.pr0_reg;
  int src2_hi = src2.pr1_reg;
  ScratchRegAlloc src1_lo_alloc = {0};
  ScratchRegAlloc src1_hi_alloc = {0};
  ScratchRegAlloc src2_lo_alloc = {0};
  ScratchRegAlloc src2_hi_alloc = {0};
  uint32_t src_exclude = 0;

  thumb_materialize_src1_for_64op(ctx, src1, src1_is64, rd_low, rd_high, &src1_lo, &src1_hi, &src1_lo_alloc,
                                  &src1_hi_alloc, &src_exclude);
  thumb_materialize_src2_for_64op(ctx, src2, src2_is64, false, &src2_lo, &src2_hi, &src2_lo_alloc, &src2_hi_alloc,
                                  &src_exclude);

  ot_check(handler.reg_handler(rd_low, src1_lo, src2_lo, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                               ENFORCE_ENCODING_NONE));

  const bool src1_high_valid = thumb_is_hw_reg(src1_hi);
  const bool src2_high_valid = thumb_is_hw_reg(src2_hi);
  if (!src1_high_valid && !src2_high_valid)
  {
    thumb_materialize_u32(rd_high, fold32(0u, 0u));
  }
  else if (!src1_high_valid || !src2_high_valid)
  {
    const int available = src1_high_valid ? src1_hi : src2_hi;
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
    ot_check(handler.reg_handler(rd_high, src1_hi, src2_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                 ENFORCE_ENCODING_NONE));
  }

thumb_logical64_cleanup:
  thumb_store_dest_pair_if_needed_ir(dest, rd_low, rd_high, store_low, store_high);
  restore_scratch_reg(&rd_high_alloc);
  restore_scratch_reg(&rd_low_alloc);
  restore_scratch_reg(&src2_hi_alloc);
  restore_scratch_reg(&src2_lo_alloc);
  restore_scratch_reg(&src1_hi_alloc);
  restore_scratch_reg(&src1_lo_alloc);
}

static void thumb_emit_shift64_imm(IROperand src1, IROperand src2, IROperand dest, TccIrOp op, const char *ctx,
                                   bool is_left, thumb_imm_handler_t dst_lo_shift, thumb_imm_handler_t dst_hi_shift,
                                   thumb_imm_handler_t cross_shift, bool sign_extend_missing_hi, bool arith_right)
{
  const uint32_t sh = (uint32_t)irop_get_imm64_ex(tcc_state->ir, src2);

  int dst_lo = dest.pr0_reg;
  int dst_hi = dest.pr1_reg;
  ScratchRegAlloc dst_lo_alloc = (ScratchRegAlloc){0};
  ScratchRegAlloc dst_hi_alloc = (ScratchRegAlloc){0};
  bool store_lo = false;
  bool store_hi = false;
  uint32_t exclude = 0;

  /* For shifts, dest might not be assigned a physical register (e.g. value lives in memory).
     Use scratch regs in that case, then store the result back. */
  thumb_prepare_dest_pair_for_64bit_op_ir(ctx, &dest, &dst_lo, &dst_hi, &dst_lo_alloc, &dst_hi_alloc, &store_lo,
                                          &store_hi, &exclude);

  int src_lo = src1.pr0_reg;
  int src_hi = src1.pr1_reg;
  ScratchRegAlloc src_lo_alloc = (ScratchRegAlloc){0};
  ScratchRegAlloc src_hi_alloc = (ScratchRegAlloc){0};
  const bool src1_is64 = irop_is_64bit(src1);

  thumb_materialize_src1_for_64op(ctx, src1, src1_is64, dst_lo, dst_hi, &src_lo, &src_hi, &src_lo_alloc, &src_hi_alloc,
                                  &exclude);

  if (src_hi == PREG_REG_NONE)
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
  thumb_store_dest_pair_if_needed_ir(dest, dst_lo, dst_hi, store_lo, store_hi);
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

static void thumb_emit_regonly_binop32(IROperand src1, IROperand src2, IROperand dest, TccIrOp op,
                                       thumb_regonly3_handler_t emitter, const char *ctx)
{
  int rd = dest.pr0_reg;
  if (rd == PREG_REG_NONE)
    tcc_error("compiler_error: %s missing destination register", ctx);
  thumb_require_materialized_reg(ctx, "dest", rd);

  /* IR-level tcc_ir_materialize_const_to_reg() now handles constant-to-register
   * conversion for register-only operations. Operands should already be in registers. */
  int rn = src1.pr0_reg;
  int rm = src2.pr0_reg;

  /* Fall back to backend materialization for VT_LVAL (memory loads) that
   * weren't handled by IR-level materialization */
  ScratchRegAlloc rn_alloc = {0};
  ScratchRegAlloc rm_alloc = {0};
  uint32_t exclude = (1u << rd);

  if (rn == PREG_REG_NONE || src1.is_lval || thumb_irop_needs_value_load(src1) || thumb_irop_has_immediate_value(src1))
  {
    rn_alloc = get_scratch_reg_with_save(exclude);
    rn = rn_alloc.reg;
    exclude |= (1u << rn);
    IROperand src1_tmp = src1;
    load_to_reg_ir(rn, PREG_NONE, src1_tmp);
  }
  else
  {
    thumb_require_materialized_reg(ctx, "src1", rn);
  }

  if (rm == PREG_REG_NONE || src2.is_lval || thumb_irop_needs_value_load(src2) || thumb_irop_has_immediate_value(src2))
  {
    rm_alloc = get_scratch_reg_with_save(exclude);
    rm = rm_alloc.reg;
    IROperand src2_tmp = src2;
    load_to_reg_ir(rm, PREG_NONE, src2_tmp);
  }
  else
  {
    thumb_require_materialized_reg(ctx, "src2", rm);
  }

  ot_check(emitter((uint32_t)rd, (uint32_t)rn, (uint32_t)rm));
  restore_scratch_reg(&rm_alloc);
  restore_scratch_reg(&rn_alloc);
}

static void thumb_emit_mod32(IROperand src1, IROperand src2, IROperand dest, TccIrOp op,
                             thumb_regonly3_handler_t div_emitter, const char *ctx)
{
  int dest_reg = dest.pr0_reg;
  if (dest_reg == PREG_REG_NONE)
    tcc_error("compiler_error: %s missing destination register", ctx);
  thumb_require_materialized_reg(ctx, "dest", dest_reg);

  /* IR-level tcc_ir_materialize_const_to_reg() now handles constant-to-register
   * conversion for register-only operations. Operands should already be in registers. */
  int src1_reg = src1.pr0_reg;
  int src2_reg = src2.pr0_reg;

  /* Fall back to backend materialization for VT_LVAL (memory loads) */
  ScratchRegAlloc src1_alloc = {0};
  ScratchRegAlloc src2_alloc = {0};
  ScratchRegAlloc quotient_alloc = {0};
  uint32_t exclude_regs = (1u << dest_reg);

  if (src1_reg == PREG_REG_NONE || src1.is_lval || thumb_irop_needs_value_load(src1) ||
      thumb_irop_has_immediate_value(src1))
  {
    src1_alloc = get_scratch_reg_with_save(exclude_regs);
    src1_reg = src1_alloc.reg;
    exclude_regs |= (1u << src1_reg);
    IROperand src1_tmp = src1;
    load_to_reg_ir(src1_reg, PREG_NONE, src1_tmp);
  }
  else
  {
    thumb_require_materialized_reg(ctx, "src1", src1_reg);
    exclude_regs |= (1u << src1_reg);
  }

  if (src2_reg == PREG_REG_NONE || src2.is_lval || thumb_irop_needs_value_load(src2) ||
      thumb_irop_has_immediate_value(src2))
  {
    src2_alloc = get_scratch_reg_with_save(exclude_regs);
    src2_reg = src2_alloc.reg;
    exclude_regs |= (1u << src2_reg);
    IROperand src2_tmp = src2;
    load_to_reg_ir(src2_reg, PREG_NONE, src2_tmp);
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

static void thumb_emit_mul32(IROperand src1, IROperand src2, IROperand dest, TccIrOp op)
{
  thumb_emit_regonly_binop32(src1, src2, dest, op, thumb_mul_regonly, "MUL");
}

typedef thumb_opcode (*thumb_longmul_handler_t)(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm);

static void thumb_emit_longmul32x32_to64(IROperand src1, IROperand src2, IROperand dest, TccIrOp op,
                                         thumb_longmul_handler_t emitter, const char *ctx)
{
  int rn = src1.pr0_reg;
  int rm = src2.pr0_reg;
  ScratchRegAlloc rn_alloc = {0};
  ScratchRegAlloc rm_alloc = {0};

  uint32_t exclude = 0;

  if (rn == PREG_REG_NONE || src1.is_lval || thumb_irop_needs_value_load(src1) || thumb_irop_has_immediate_value(src1))
  {
    rn_alloc = get_scratch_reg_with_save(exclude);
    rn = rn_alloc.reg;
    exclude |= (1u << rn);
    IROperand src1_tmp = src1;
    load_to_reg_ir(rn, PREG_NONE, src1_tmp);
  }
  else
  {
    thumb_require_materialized_reg(ctx, "src1", rn);
    if (thumb_is_hw_reg(rn))
      exclude |= (1u << rn);
  }

  if (rm == PREG_REG_NONE || src2.is_lval || thumb_irop_needs_value_load(src2) || thumb_irop_has_immediate_value(src2))
  {
    rm_alloc = get_scratch_reg_with_save(exclude);
    rm = rm_alloc.reg;
    exclude |= (1u << rm);
    IROperand src2_tmp = src2;
    load_to_reg_ir(rm, PREG_NONE, src2_tmp);
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
  int rd_low = dest.pr0_reg;
  int rd_high = dest.pr1_reg;

  thumb_prepare_dest_pair_for_64bit_op_ir(ctx, &dest, &rd_low, &rd_high, &rd_low_alloc, &rd_high_alloc, &store_low,
                                          &store_high, &exclude);

  ot_check(emitter(rd_low, rd_high, rn, rm));

  thumb_store_dest_pair_if_needed_ir(dest, rd_low, rd_high, store_low, store_high);
  restore_scratch_reg(&rd_high_alloc);
  restore_scratch_reg(&rd_low_alloc);
  restore_scratch_reg(&rm_alloc);
  restore_scratch_reg(&rn_alloc);
}

static void thumb_process_data64_op(IROperand src1, IROperand src2, IROperand dest, TccIrOp op)
{
  ThumbDataProcessingHandler regular_handler;
  ThumbDataProcessingHandler carry_handler;
  const char *context = "unk";
  switch (op)
  {
  case TCCIR_OP_UMULL:
  {
    thumb_emit_longmul32x32_to64(src1, src2, dest, op, th_umull, "UMULL");
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
    if (!thumb_irop_has_immediate_value(src2))
      tcc_error("compiler_error: 64-bit SHL expects immediate shift count");
    thumb_emit_shift64_imm(src1, src2, dest, op, "64-bit SHL", true, th_lsl_imm, th_lsl_imm, th_lsr_imm, false, false);
    return;
  }
  case TCCIR_OP_SHR:
  {
    if (!thumb_irop_has_immediate_value(src2))
      tcc_error("compiler_error: 64-bit SHR expects immediate shift count");
    thumb_emit_shift64_imm(src1, src2, dest, op, "64-bit SHR", false, th_lsr_imm, th_lsr_imm, th_lsl_imm, false, false);
    return;
  }
  case TCCIR_OP_SAR:
  {
    if (!thumb_irop_has_immediate_value(src2))
      tcc_error("compiler_error: 64-bit SAR expects immediate shift count");
    thumb_emit_shift64_imm(src1, src2, dest, op, "64-bit SAR", false, th_lsr_imm, th_asr_imm, th_lsl_imm, true, true);
    return;
  }
  case TCCIR_OP_OR:
  {
    ThumbDataProcessingHandler logical;
    logical.imm_handler = th_orr_imm;
    logical.reg_handler = th_orr_reg;
    return thumb_emit_logical64_op(src1, src2, dest, op, logical, thumb_fold_u64_or, thumb_fold_u32_or, "64-bit OR");
  }
  case TCCIR_OP_AND:
  {
    ThumbDataProcessingHandler logical;
    logical.imm_handler = th_and_imm;
    logical.reg_handler = th_and_reg;
    return thumb_emit_logical64_op(src1, src2, dest, op, logical, thumb_fold_u64_and, thumb_fold_u32_and, "64-bit AND");
  }
  break;
  case TCCIR_OP_XOR:
  {
    ThumbDataProcessingHandler logical;
    logical.imm_handler = th_eor_imm;
    logical.reg_handler = th_eor_reg;
    return thumb_emit_logical64_op(src1, src2, dest, op, logical, thumb_fold_u64_xor, thumb_fold_u32_xor, "64-bit XOR");
  }
  break;
  default:
    tcc_error("compiler_error: unsupported 64-bit data processing operation: %d", op);
    break;
  }

  return thumb_emit_opcode64_imm_ir(src1, src2, dest, op, context, regular_handler, carry_handler);
}

static void thumb_emit_data_processing_op32(IROperand src1, IROperand src2, IROperand dest, TccIrOp op,
                                            ThumbDataProcessingHandler handler, thumb_flags_behaviour flags)
{
  const char *ctx = tcc_ir_get_op_name(op);

  int src1_reg = src1.pr0_reg;
  int src2_reg = src2.pr0_reg;

  const bool src1_is_imm = thumb_irop_has_immediate_value(src1);
  const bool src2_is_imm = thumb_irop_has_immediate_value(src2);

  const bool src1_needs_load =
      src1_is_imm || thumb_irop_needs_value_load(src1) || src1.is_lval || src1_reg == PREG_REG_NONE;
  const bool src2_needs_load =
      src2_is_imm || thumb_irop_needs_value_load(src2) || src2.is_lval || src2_reg == PREG_REG_NONE;

  uint32_t exclude_regs = 0;
  ScratchRegAlloc src1_alloc = {0};
  ScratchRegAlloc src2_alloc = {0};

  const bool dest_sets_flags = (op == TCCIR_OP_CMP);
  int dest_reg = PREG_NONE;
  if (irop_is_none(dest))
  {
    if (!dest_sets_flags)
      tcc_error("compiler_error: %s requires a destination", ctx);
    /* CMP only sets flags; the encoding ignores Rd. Use R0 to keep encoders happy. */
    dest_reg = R0;
  }
  else
  {
    dest_reg = dest.pr0_reg;
    if (dest_reg == PREG_REG_NONE)
    {
      if (!dest_sets_flags)
        tcc_error("compiler_error: %s missing destination register after materialization", ctx);
      /* CMP only sets flags; the encoding ignores Rd. Use R0 to keep encoders happy. */
      dest_reg = R0;
    }
    else
    {
      thumb_require_materialized_reg(ctx, "dest", dest_reg);
      if (thumb_is_hw_reg(dest_reg))
        exclude_regs |= (1u << dest_reg);
    }
  }

  /* If src2 is already in a register, exclude it too so src1 doesn't clobber it */
  if (!src2_is_imm && !thumb_irop_needs_value_load(src2) && !src2.is_lval && thumb_is_hw_reg(src2_reg))
  {
    exclude_regs |= (1u << src2_reg);
  }

  if (src1_needs_load)
  {
    src1_alloc = get_scratch_reg_with_save(exclude_regs);
    src1_reg = src1_alloc.reg;
    if (thumb_is_hw_reg(src1_reg))
      exclude_regs |= (1u << src1_reg);
    IROperand src1_tmp = src1;
    load_to_reg_ir(src1_reg, PREG_NONE, src1_tmp);
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
    const uint32_t imm_val = (uint32_t)irop_get_imm64_ex(tcc_state->ir, src2);
    if (handler.imm_handler && ot(handler.imm_handler(dest_reg, src1_reg, imm_val, flags, ENFORCE_ENCODING_NONE)))
    {
      if (src1_alloc.reg != 0)
        restore_scratch_reg(&src1_alloc);
      return;
    }

    src2_alloc = get_scratch_reg_with_save(exclude_regs);
    src2_reg = src2_alloc.reg;
    IROperand src2_tmp = src2;
    load_to_reg_ir(src2_reg, PREG_NONE, src2_tmp);
  }
  else if (src2_needs_load)
  {
    src2_alloc = get_scratch_reg_with_save(exclude_regs);
    src2_reg = src2_alloc.reg;
    IROperand src2_tmp = src2;
    load_to_reg_ir(src2_reg, PREG_NONE, src2_tmp);
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

void tcc_gen_machine_data_processing_op(IROperand src1, IROperand src2, IROperand dest, TccIrOp op)
{
  ThumbDataProcessingHandler handler;
  thumb_flags_behaviour flags = FLAGS_BEHAVIOUR_NOT_IMPORTANT;

  /* Check for 64-bit operations.
   * UMULL always produces a 64-bit result from 32-bit inputs, so it must
   * always use the 64-bit handler regardless of the dest type annotation. */
  if (!irop_is_none(dest) && (irop_is_64bit(dest) || op == TCCIR_OP_UMULL))
  {
    return thumb_process_data64_op(src1, src2, dest, op);
  }

  /* NOTE: All spilled register loading is now handled centrally in generate_code via
   * tcc_ir_materialize_value()/materialize_dest(). This function receives valid
   * physical registers in pr0/pr1 (no PREG_SPILLED sentinels). */

  switch (op)
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
    thumb_emit_mul32(src1, src2, dest, op);
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
    thumb_emit_regonly_binop32(src1, src2, dest, op, thumb_sdiv_regonly, "DIV");
    return;
  }
  case TCCIR_OP_UDIV:
  {
    thumb_emit_regonly_binop32(src1, src2, dest, op, thumb_udiv_regonly, "UDIV");
    return;
  }
  case TCCIR_OP_IMOD:
  {
    thumb_emit_mod32(src1, src2, dest, op, thumb_sdiv_regonly, "IMOD");
    return;
  }
  case TCCIR_OP_UMOD:
  {
    thumb_emit_mod32(src1, src2, dest, op, thumb_udiv_regonly, "UMOD");
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
    const int is64 = irop_is_64bit(src1);
    int src_lo = src1.pr0_reg;
    int src_hi = src1.pr1_reg;

    /* Handle immediate constant, missing register(s), or lvalue (needs dereference).
     * When VT_LVAL is set, the register holds an address and we need to load
     * the value it points to before comparing against zero. */
    const int needs_load = thumb_irop_has_immediate_value(src1) || src_lo == PREG_REG_NONE || src1.is_lval ||
                           thumb_irop_needs_value_load(src1) || (is64 && src_hi == PREG_REG_NONE);

    if (!is64)
    {
      ScratchRegAlloc src_alloc = {0};
      if (needs_load)
      {
        src_alloc = get_scratch_reg_with_save(0);
        src_lo = src_alloc.reg;
        IROperand src1_tmp = src1;
        load_to_reg_ir(src_lo, PREG_NONE, src1_tmp);
      }
      else
      {
        thumb_require_materialized_reg("TEST_ZERO", "src", src_lo);
      }

      ot_check(th_cmp_imm(0, src_lo, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));

      if (src_alloc.reg != 0)
        restore_scratch_reg(&src_alloc);
      return;
    }

    /* 64-bit: Z must be set iff (lo == 0 && hi == 0).
     * Use CMP lo,#0; IT EQ; CMPEQ hi,#0 so if lo!=0 we keep Z=0. */
    TCCMachineScratchRegs scratch;
    memset(&scratch, 0, sizeof(scratch));
    int used_scratch = 0;
    if (needs_load)
    {
      used_scratch = 1;
      tcc_machine_acquire_scratch(&scratch, TCC_MACHINE_SCRATCH_NEEDS_PAIR);
      src_lo = scratch.regs[0];
      src_hi = scratch.regs[1];
      IROperand src1_tmp = src1;
      load_to_reg_ir(src_lo, src_hi, src1_tmp);
    }
    else
    {
      thumb_require_materialized_reg("TEST_ZERO", "src_lo", src_lo);
      thumb_require_materialized_reg("TEST_ZERO", "src_hi", src_hi);
    }

    ot_check(th_cmp_imm(0, src_lo, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
    ot_check(th_it(mapcc(TOK_EQ), 0x8)); /* IT EQ (single instruction) */
    ot_check(th_cmp_imm(0, src_hi, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));

    if (used_scratch)
      tcc_machine_release_scratch(&scratch);
    return;
  }
  default:
  {
    printf("compiler_error: unhandled data processing op: %s\n", tcc_ir_get_op_name(op));
    return;
  }
  }

  thumb_emit_data_processing_op32(src1, src2, dest, op, handler, flags);
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

static void gen_softfp_call(IROperand src1, IROperand src2, IROperand dest, TccIrOp op, const char *func_name,
                            int is_double)
{
  Sym *sym;
  IROperand func_op;

  /* Load operands into argument registers per soft-float EABI convention */
  if (op == TCCIR_OP_FNEG)
  {
    /* Unary: single operand in R0 (float) or R0:R1 (double) */
    load_to_reg_ir(R0, is_double ? R1 : PREG_NONE, src1);
  }
  else if (op == TCCIR_OP_FCMP)
  {
    /* Binary comparison: src1 in R0/R0:R1, src2 in R1/R2:R3 */
    if (is_double)
    {
      load_to_reg_ir(R0, R1, src1);
      load_to_reg_ir(R2, R3, src2);
    }
    else
    {
      load_to_reg_ir(R0, PREG_NONE, src1);
      load_to_reg_ir(R1, PREG_NONE, src2);
    }
  }
  else if (op == TCCIR_OP_CVT_FTOF || op == TCCIR_OP_CVT_ITOF || op == TCCIR_OP_CVT_FTOI)
  {
    /* Conversion: single operand in R0 (float/int) or R0:R1 (double/long) */
    int src_is_64bit = irop_is_64bit(src1);
    load_to_reg_ir(R0, src_is_64bit ? R1 : PREG_NONE, src1);
  }
  else
  {
    /* Binary arithmetic: src1 in R0/R0:R1, src2 in R1/R2:R3 */
    if (is_double)
    {
      load_to_reg_ir(R0, R1, src1);
      load_to_reg_ir(R2, R3, src2);
    }
    else
    {
      load_to_reg_ir(R0, PREG_NONE, src1);
      load_to_reg_ir(R1, PREG_NONE, src2);
    }
  }

  /* Get or create the external symbol for the soft-float function */
  sym = external_global_sym(tok_alloc_const(func_name), &func_old_type);

  /* Set up IROperand for the function call */
  uint32_t sym_idx = tcc_ir_pool_add_symref(tcc_state->ir, sym, 0, 0);
  func_op = irop_make_symref(-1, sym_idx, 0, 0, 1, IROP_BTYPE_FUNC);

  /* Generate BL to the soft-float function */
  gcall_or_jump_ir(0, func_op);

  /* Result is in R0 (float/int) or R0:R1 (double/long) */
  if (op != TCCIR_OP_FCMP)
  {
    if (irop_is_64bit(dest))
    {
      /* For 64-bit results, R0 holds low word, R1 holds high word. */
      if (dest.pr0_reg != PREG_REG_NONE || dest.pr1_reg != PREG_REG_NONE)
      {
        if (dest.pr0_reg == PREG_REG_NONE || dest.pr1_reg == PREG_REG_NONE)
          tcc_error("compiler_error: soft-float double result destination missing register half");
        if (dest.pr0_spilled || dest.pr1_spilled)
          tcc_error("compiler_error: soft-float double result destination unexpectedly spilled");
        thumb_require_materialized_reg("gen_softfp_call", "dest.low", dest.pr0_reg);
        thumb_require_materialized_reg("gen_softfp_call", "dest.high", dest.pr1_reg);
        if (dest.pr0_reg != R0)
        {
          ot_check(th_mov_reg(dest.pr0_reg, R0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE, false));
        }
        if (dest.pr1_reg != R1)
        {
          ot_check(th_mov_reg(dest.pr1_reg, R1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE, false));
        }
      }
      else
      {
        /* Memory destination: store both words using store_ir(). */
        IROperand dest_with_r1 = dest;
        dest_with_r1.pr1_reg = R1;
        store_ir(R0, dest_with_r1);
      }
    }
    else
    {
      store_ir(R0, dest);
    }
  }
  /* For FCMP, result is in CPSR flags - no store needed */
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

/* Soft float negation: XOR the sign bit.
 * For float: XOR R0 with 0x80000000
 * For double: XOR R1 with 0x80000000 (high word has sign)
 */
static void gen_softfp_fneg(IROperand src1, IROperand dest, int is_double)
{
  int xor_reg = is_double ? R1 : R0;
  ScratchRegAlloc scratch_alloc;
  int scratch_reg;

  load_to_reg_ir(R0, is_double ? R1 : PREG_NONE, src1);

  scratch_alloc = get_scratch_reg_with_save((1 << R0) | (is_double ? (1 << R1) : 0));
  scratch_reg = scratch_alloc.reg;
  load_full_const(scratch_reg, PREG_NONE, 0x80000000, NULL);

  ot_check(th_eor_reg(xor_reg, xor_reg, scratch_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                      ENFORCE_ENCODING_NONE));

  restore_scratch_reg(&scratch_alloc);
  store_ir(R0, dest);
}

/* Soft float comparison using __aeabi_cfcmple / __aeabi_cdcmple.
 * These set CPSR flags directly for subsequent SETIF/JUMPIF.
 */
static void gen_softfp_fcmp(IROperand src1, IROperand src2, int is_double)
{
  const char *cmp_func = is_double ? "__aeabi_cdcmple" : "__aeabi_cfcmple";
  Sym *sym;
  IROperand func_op;

  if (is_double)
  {
    load_to_reg_ir(R0, R1, src1);
    load_to_reg_ir(R2, R3, src2);
  }
  else
  {
    load_to_reg_ir(R0, PREG_NONE, src1);
    load_to_reg_ir(R1, PREG_NONE, src2);
  }

  sym = external_global_sym(tok_alloc_const(cmp_func), &func_old_type);

  uint32_t sym_idx = tcc_ir_pool_add_symref(tcc_state->ir, sym, 0, 0);
  func_op = irop_make_symref(-1, sym_idx, 0, 0, 1, IROP_BTYPE_FUNC);

  gcall_or_jump_ir(0, func_op);
}

/* Get soft float function name for float<->double conversion */
static const char *get_softfp_cvt_ftof_func_name(IROperand src1, IROperand dest)
{
  int src_is_double = (irop_get_btype(src1) == IROP_BTYPE_FLOAT64);
  int dst_is_double = (irop_get_btype(dest) == IROP_BTYPE_FLOAT64);

  if (dst_is_double && !src_is_double)
    return "__aeabi_f2d";
  if (!dst_is_double && src_is_double)
    return "__aeabi_d2f";
  return NULL; /* same type, no conversion needed */
}

/* Get soft float function name for int->float conversion */
static const char *get_softfp_cvt_itof_func_name(IROperand src1, IROperand dest)
{
  int src_is_64bit = (irop_get_btype(src1) == IROP_BTYPE_INT64);
  int dst_is_double = (irop_get_btype(dest) == IROP_BTYPE_FLOAT64);
  int is_unsigned = src1.is_unsigned;

  if (src_is_64bit)
    return is_unsigned ? (dst_is_double ? "__aeabi_ul2d" : "__aeabi_ul2f")
                       : (dst_is_double ? "__aeabi_l2d" : "__aeabi_l2f");
  return is_unsigned ? (dst_is_double ? "__aeabi_ui2d" : "__aeabi_ui2f")
                     : (dst_is_double ? "__aeabi_i2d" : "__aeabi_i2f");
}

/* Get soft float function name for float->int conversion */
static const char *get_softfp_cvt_ftoi_func_name(IROperand src1, IROperand dest)
{
  int src_is_double = (irop_get_btype(src1) == IROP_BTYPE_FLOAT64);
  int dst_is_64bit = (irop_get_btype(dest) == IROP_BTYPE_INT64);
  int is_unsigned = dest.is_unsigned;

  if (dst_is_64bit)
    return is_unsigned ? (src_is_double ? "__aeabi_d2ulz" : "__aeabi_f2ulz")
                       : (src_is_double ? "__aeabi_d2lz" : "__aeabi_f2lz");
  return is_unsigned ? (src_is_double ? "__aeabi_d2uiz" : "__aeabi_f2uiz")
                     : (src_is_double ? "__aeabi_d2iz" : "__aeabi_f2iz");
}

/* Generate floating point operation.
 * Uses VFP hardware instructions when available,
 * otherwise falls back to software library calls.
 */
ST_FUNC void tcc_gen_machine_fp_op(IROperand dest, IROperand src1, IROperand src2, TccIrOp op)
{
  const int is_double = irop_is_64bit(src1);
  // int use_vfp = can_use_vfp(is_double);
  const char *func_name;

  /* VFP hardware path */
  // if (use_vfp)
  // {
  //   switch (op)
  //   {
  //   case TCCIR_OP_FCMP:
  //     gen_hardfp_cmp(src1, src2, dest, op, is_double);
  //     return;
  //   case TCCIR_OP_FADD:
  //   case TCCIR_OP_FSUB:
  //   case TCCIR_OP_FMUL:
  //   case TCCIR_OP_FDIV:
  //   case TCCIR_OP_FNEG:
  //     gen_hardfp_op(src1, src2, dest, op, is_double);
  //     return;
  //   case TCCIR_OP_CVT_FTOF:
  //     gen_hardfp_cvt_ftof(src1, dest, op);
  //     return;
  //   case TCCIR_OP_CVT_ITOF:
  //     gen_hardfp_cvt_itof(src1, dest, op);
  //     return;
  //   case TCCIR_OP_CVT_FTOI:
  //     gen_hardfp_cvt_ftoi(src1, dest, op);
  //     return;
  //   default:
  //     break;
  //   }
  // }

  /* Software floating point path */
  switch (op)
  {
  case TCCIR_OP_FNEG:
    gen_softfp_fneg(src1, dest, is_double);
    return;

  case TCCIR_OP_FCMP:
    gen_softfp_fcmp(src1, src2, is_double);
    return;

  case TCCIR_OP_CVT_FTOF:
    func_name = get_softfp_cvt_ftof_func_name(src1, dest);
    if (!func_name)
    {
      /* Same type, no conversion needed - just copy */
      int src_is_double = irop_is_64bit(src1);
      load_to_reg_ir(R0, src_is_double ? R1 : PREG_NONE, src1);
      store_ex_ir(R0, dest, 0);
      return;
    }
    gen_softfp_call(src1, src2, dest, op, func_name, is_double);
    return;

  case TCCIR_OP_CVT_ITOF:
    func_name = get_softfp_cvt_itof_func_name(src1, dest);
    gen_softfp_call(src1, src2, dest, op, func_name, 0);
    return;

  case TCCIR_OP_CVT_FTOI:
    func_name = get_softfp_cvt_ftoi_func_name(src1, dest);
    gen_softfp_call(src1, src2, dest, op, func_name, is_double);
    return;

  default:
    /* Arithmetic ops (FADD, FSUB, FMUL, FDIV) */
    func_name = get_softfp_func_name(op, is_double);
    if (func_name)
    {
      gen_softfp_call(src1, src2, dest, op, func_name, is_double);
      return;
    }
    break;
  }

  tcc_error("compiler_error: unknown FP operation in tcc_gen_machine_fp_op");
}

ST_FUNC void tcc_gen_machine_return_value_op(IROperand src, TccIrOp op)
{
  const int is_64bit = irop_is_64bit(src);

  /* Constants are not held in a physical register; always materialize them
   * into the return registers, regardless of any (possibly stale) pr0/pr1
   * fields. */
  if (src.is_const)
  {
    Sym *sym = irop_get_sym(src);
    tcc_machine_load_constant(R0, is_64bit ? R1 : PREG_NONE, src.u.imm32, is_64bit, sym);
    return;
  }

  /* NOTE: src1 is preloaded to a valid register by generate_code if it was spilled.
   * Just move to return registers R0 (and R1 for 64-bit). */
  if (src.pr0_reg != PREG_REG_NONE)
  {
    /* If still marked as spilled here, something went wrong with materialization */
    if (src.pr0_spilled)
      tcc_error("compiler_error: return value source unexpectedly still spilled");
    load_to_register_ir(R0, src.pr0_reg, src);
    if (is_64bit && src.pr1_reg != PREG_REG_NONE)
    {
      if (src.pr1_spilled)
        tcc_error("compiler_error: return value source high half unexpectedly still spilled");
      load_to_register_ir(R1, src.pr1_reg, src);
    }
    return;
  }

  /* If we get here with invalid pr0, handle constant case */
  IROperand dest = irop_make_none();
  dest.pr0_reg = R0;
  dest.pr0_spilled = 0;
  dest.pr1_reg = is_64bit ? R1 : PREG_REG_NONE;
  dest.pr1_spilled = 0;
  load_to_dest_ir(dest, src);
}

ST_FUNC void tcc_gen_machine_load_op(IROperand dest, IROperand src)
{
  TRACE("'tcc_gen_machine_load_op'");

  load_to_dest_ir(dest, src);
}

ST_FUNC void tcc_gen_machine_store_op(IROperand dest, IROperand src, TccIrOp op)
{
  if (irop_is_none(src))
  {
    tcc_error("compiler_error: NULL src in tcc_gen_machine_store_op");
  }
  if (irop_is_none(dest))
  {
    tcc_error("compiler_error: NULL dest in tcc_gen_machine_store_op");
  }
  TRACE("'tcc_gen_machine_store_op'");
  const char *ctx = "tcc_gen_machine_store_op";
  int src_reg;
  /* Check for 64-bit types - include VT_LLONG for soft-float doubles and long
   * long */
  const int is_64bit = irop_is_64bit(src);

  src_reg = src.pr0_reg;
  ScratchRegAlloc scratch_alloc = {0};

  /* If src_reg is missing, spilled, or src isn't a direct register value (const/lvalue), reload it. */
  const int src_is_const = src.is_const;
  const int src_is_lval = src.is_lval;
  const int src_is_spilled = (src_reg != PREG_REG_NONE) && src.pr0_spilled;
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
    load_to_reg_ir(src_reg, is_64bit ? R11 : PREG_NONE, src);

    if (is_64bit)
    {
      dest.pr1_reg = R11;
      dest.pr1_spilled = 0;
    }
    store_ex_ir(src_reg, dest, 0);
  }
  else
  {
    if (is_64bit)
    {
      dest.pr1_reg = src.pr1_reg;
      dest.pr1_spilled = src.pr1_spilled;
      const uint8_t pr1_packed = (dest.pr1_spilled ? PREG_SPILLED : 0) | dest.pr1_reg;
      if (pr1_packed != PREG_NONE)
        thumb_require_materialized_reg(ctx, "src.high", pr1_packed);
    }
    store_ex_ir(src_reg, dest, 0);
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
  TCCIRState *ir = tcc_state->ir;

  if (!leaffunc)
  {
    registers_to_push |= (1 << R_LR);
    registers_count++;
  }

  /* Variadic functions need a stable FP for va_list setup. */
  if (func_var)
  {
    tcc_state->need_frame_pointer = 1;
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
  /* Variadic save area is reserved in the IR stack layout (loc bias). */

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

  /* For variadic functions, save incoming r0-r3 in a fixed area at FP-16..FP-4
   * and store the caller stack-args pointer at FP-20.
   */
  int named_reg_bytes = 0;
  int named_stack_bytes = 0;
  if (func_var && ir)
  {
    named_reg_bytes = ir->named_arg_reg_bytes;
    named_stack_bytes = ir->named_arg_stack_bytes;
  }

  if (func_var)
  {
    tcc_gen_machine_store_to_stack(R0, -16);
    tcc_gen_machine_store_to_stack(R1, -12);
    tcc_gen_machine_store_to_stack(R2, -8);
    tcc_gen_machine_store_to_stack(R3, -4);

    /* stack args start at FP + offset_to_args + named_stack_bytes */
    ot_check(th_add_imm(R12, R_FP, offset_to_args + named_stack_bytes, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                        ENFORCE_ENCODING_NONE));
    tcc_gen_machine_store_to_stack(R12, -20);

    /* store the number of named-arg bytes consumed in r0-r3 */
    tcc_machine_load_constant(R12, PREG_NONE, named_reg_bytes, 0, NULL);
    tcc_gen_machine_store_to_stack(R12, -24);
  }

  /* Move parameters from incoming registers to their allocated locations.
   * For non-leaf functions or parameters that cross calls:
   * - If allocated to callee-saved register: move from R0-R3 to allocated reg
   * - If spilled: store from R0-R3 to stack location
   * For leaf functions with params staying in R0-R3: no move needed */
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

/* Helper: assign to 64-bit destination */
static void assign_op_64bit(IROperand dest, IROperand src)
{
  const int src_is_64bit = irop_is_64bit(src);
  const int dest_in_mem = dest.is_lval;

  int src_lo = src.pr0_reg;
  int src_hi = src_is_64bit ? src.pr1_reg : PREG_REG_NONE;
  ScratchRegAlloc src_lo_alloc = {0};
  ScratchRegAlloc src_hi_alloc = {0};

  /* Check for spilled sources - these need to be loaded to registers */
  const int src_lo_spilled = (src_lo != PREG_REG_NONE) && src.pr0_spilled;
  const int src_hi_spilled = (src_hi != PREG_REG_NONE) && src.pr1_spilled;

  /* Materialize source into registers if needed (const/spilled/lvalue/etc).
   * If either half is spilled, reload the whole 64-bit value.
   * Check tag for true constants to avoid misinterpreting vregs with stale is_const flag. */
  int src_tag = irop_get_tag(src);
  int src_is_imm = (src_tag == IROP_TAG_IMM32 || src_tag == IROP_TAG_I64 || src_tag == IROP_TAG_F32 ||
                    src_tag == IROP_TAG_F64 || src_tag == IROP_TAG_SYMREF || src_tag == IROP_TAG_STACKOFF);
  if (src_is_imm || src.is_lval || src_lo == PREG_REG_NONE || src_lo_spilled || (src_is_64bit && src_hi_spilled))
  {
    uint32_t exclude = 0;
    if (!dest_in_mem)
    {
      if (dest.pr0_reg != PREG_REG_NONE && !dest.pr0_spilled && dest.pr0_reg <= 15)
        exclude |= (1u << dest.pr0_reg);
      if (dest.pr1_reg != PREG_REG_NONE && !dest.pr1_spilled && dest.pr1_reg <= 15)
        exclude |= (1u << dest.pr1_reg);
    }
    src_lo_alloc = get_scratch_reg_with_save(exclude);
    exclude |= (1u << src_lo_alloc.reg);
    if (src_is_64bit)
    {
      src_hi_alloc = get_scratch_reg_with_save(exclude);
      load_to_reg_ir(src_lo_alloc.reg, src_hi_alloc.reg, src);
      src_hi = src_hi_alloc.reg;
    }
    else
    {
      load_to_reg_ir(src_lo_alloc.reg, PREG_REG_NONE, src);
      src_hi = PREG_REG_NONE;
    }
    src_lo = src_lo_alloc.reg;
  }
  else if (src_hi == PREG_REG_NONE)
  {
    /* Mixed 32->64 promotion: treat missing high word as 0. */
    uint32_t exclude = 0;
    if (!dest_in_mem)
    {
      if (dest.pr0_reg != PREG_REG_NONE && !dest.pr0_spilled && dest.pr0_reg <= 15)
        exclude |= (1u << dest.pr0_reg);
      if (dest.pr1_reg != PREG_REG_NONE && !dest.pr1_spilled && dest.pr1_reg <= 15)
        exclude |= (1u << dest.pr1_reg);
    }
    if (src_lo != PREG_REG_NONE && src_lo <= 15)
      exclude |= (1u << src_lo);
    src_hi_alloc = get_scratch_reg_with_save(exclude);
    ot_check(th_mov_imm(src_hi_alloc.reg, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    src_hi = src_hi_alloc.reg;
  }

  if (dest_in_mem)
  {
    /* Store low and high words separately as 32-bit stores.
     * When storing the low word, exclude src_hi from scratch allocation
     * to prevent clobbering the high word value before it's stored. */
    IROperand dest_lo = dest;
    dest_lo.btype = IROP_BTYPE_INT32;
    IROperand dest_hi = dest_lo;
    dest_hi.u.imm32 += 4;

    store_ex_ir(src_lo, dest_lo, (1u << src_hi));
    store_ir(src_hi, dest_hi);
  }
  else
  {
    if (dest.pr0_reg != src_lo && dest.pr0_reg != PREG_REG_NONE && src_lo != PREG_REG_NONE)
    {
      ot_check(th_mov_reg(dest.pr0_reg, src_lo, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE, false));
    }
    if (dest.pr1_reg != src_hi && dest.pr1_reg != PREG_REG_NONE && src_hi != PREG_REG_NONE)
    {
      ot_check(th_mov_reg(dest.pr1_reg, src_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE, false));
    }
  }

  restore_scratch_reg(&src_hi_alloc);
  restore_scratch_reg(&src_lo_alloc);
}

ST_FUNC void tcc_gen_machine_assign_op(IROperand dest, IROperand src, TccIrOp op)
{
  const int dest_is_64bit = irop_is_64bit(dest);

  /* 64-bit destination has dedicated handler */
  if (dest_is_64bit)
  {
    assign_op_64bit(dest, src);
    return;
  }

  int tag = irop_get_tag(src);
  int is_imm_const = (tag == IROP_TAG_IMM32 || tag == IROP_TAG_I64 || tag == IROP_TAG_F32 || tag == IROP_TAG_F64);

  if (is_imm_const && !src.is_lval)
  {
    Sym *sym = irop_get_sym_ex(tcc_state->ir, src);
    int64_t src_imm = irop_get_imm64_ex(tcc_state->ir, src);

    if (dest.is_lval && (dest.is_local || dest.is_const))
    {
      ScratchRegAlloc scratch_alloc = get_scratch_reg_with_save(0);
      tcc_machine_load_constant(scratch_alloc.reg, PREG_NONE, src_imm, 0, sym);
      IROperand dest_direct = dest;
      dest_direct.is_lval = 0;
      store_ir(scratch_alloc.reg, dest_direct);
      restore_scratch_reg(&scratch_alloc);
    }
    else
    {
      tcc_machine_load_constant(dest.pr0_reg, dest_is_64bit ? dest.pr1_reg : PREG_REG_NONE, src_imm, dest_is_64bit,
                                sym);
    }
    return;
  }

  /* Symbol dereference (SYMREF with is_lval) */
  if ((src.is_sym || tag == IROP_TAG_SYMREF) && src.is_lval)
  {
    if (dest.is_lval && (dest.is_local || dest.is_const))
    {
      ScratchRegAlloc scratch_alloc = get_scratch_reg_with_save(0);
      load_to_reg_ir(scratch_alloc.reg, PREG_REG_NONE, src);
      IROperand dest_direct = dest;
      dest_direct.is_lval = 0;
      store_ir(scratch_alloc.reg, dest_direct);
      restore_scratch_reg(&scratch_alloc);
    }
    else
    {
      load_to_reg_ir(dest.pr0_reg, dest_is_64bit ? dest.pr1_reg : PREG_REG_NONE, src);
    }
    return;
  }

  /* Symbol address, local address, or memory load - load_to_dest_ir handles all */
  if ((src.is_sym || tag == IROP_TAG_SYMREF) || src.is_local || src.is_lval)
  {
    load_to_dest_ir(dest, src);
    return;
  }

  /* Same register - nothing to do */
  if (dest.pr0_reg == src.pr0_reg && dest.pr0_spilled == src.pr0_spilled)
    return;

  /* Register to register move */
  ot_check(th_mov_reg(dest.pr0_reg, src.pr0_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                      ENFORCE_ENCODING_NONE, false));
}

/* Load Effective Address: compute the address of src1 into dest.
 * This is the explicit "address-of" operation for local variables/arrays.
 * Unlike LOAD which dereferences, LEA computes FP+offset into a register.
 */
ST_FUNC void tcc_gen_machine_lea_op(IROperand dest, IROperand src, TccIrOp op)
{
  const char *ctx = "tcc_gen_machine_lea_op";
  int dest_reg = dest.pr0_reg;
  // int src_v = src1->r & VT_VALMASK;

  /* IR owns spills: LEA destination must already be materialized. */
  thumb_require_materialized_reg(ctx, "dest", dest_reg);

  if (src.is_local || src.is_llocal)
  {
    /* Compute address of local: FP + offset */
    int base = R_FP;
    if (tcc_state->need_frame_pointer == 0)
      base = R_SP;

    /* For local variables (VAR vregs), use the original offset from c.i.
     * The register allocator may have assigned a different spill slot,
     * but for address-of operations we need the original variable location.
     * For spilled temps/params, use the allocated stack slot offset.
     */
    int offset;
    const int vreg_type = TCCIR_DECODE_VREG_TYPE(src.vr);
    if (vreg_type == TCCIR_VREG_TYPE_VAR && src.u.imm32 != 0)
    {
      /* VAR vreg with non-zero c.i: use original variable offset */
      offset = src.u.imm32;
    }
    else
    {
      /* Use vreg-based stack slot offset if available, otherwise fall back to c.i */
      const TCCStackSlot *slot = tcc_ir_stack_slot_by_vreg(tcc_state->ir, src.vr);
      if (slot)
        offset = slot->offset;
      else
        offset = (int)src.u.imm32;
    }
    /* Stack parameters live above the saved-register area.
     * When computing their address, fold in offset_to_args (prologue push size).
     * EXCEPTION: Variadic register parameters are saved in the prologue at
     * negative offsets (FP-16 to FP-4), so they're already in our local frame
     * and should NOT have offset_to_args added. */
    if (src.is_param && offset >= 0)
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
  else if (src.is_const && src.is_sym)
  {
    /* Address of global symbol */
    Sym *sym = irop_get_sym(src);
    load_full_const(dest_reg, PREG_NONE, src.u.imm32, sym);
  }
  else
  {
    /* Fallback: if src is already in a register, just move it */
    const int src_reg = src.pr0_reg;
    if (src_reg != PREG_REG_NONE)
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
      tcc_error("compiler_error: LEA on unexpected operand type");
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

static void gcall_or_jump_ir(int is_jmp, IROperand dest)
{
  const int tag = irop_get_tag(dest);

  if ((tag == IROP_TAG_IMM32 || tag == IROP_TAG_SYMREF) && !dest.is_lval)
  {
    /* IMPORTANT: ot_check() may flush a pending literal pool *before* emitting
     * this BL, which inserts a pool skip-branch at the current `ind`.
     * If we record the relocation at `ind` before ot_check(), the linker will
     * patch the pool skip-branch instead of the BL (corrupting control flow).
     *
     * Therefore: emit first, then record relocation at the actual BL position.
     */
    Sym *sym = NULL;
    Sym *validated_sym = NULL;
    Sym *reloc_sym = NULL;
    int32_t addend = 0;
    if (tag == IROP_TAG_SYMREF)
    {
      IRPoolSymref *symref = irop_get_symref_ex(tcc_state->ir, dest);
      sym = symref ? symref->sym : NULL;
      addend = symref ? symref->addend : 0;
      validated_sym = sym ? validate_sym_for_reloc(sym) : NULL;
      /* If symbol is not yet registered, try to externalize it so relocation works.
       * This mirrors load_full_const() behavior for literal pools. */
      if (sym && !validated_sym && !(sym->v & SYM_FIELD))
      {
        put_extern_sym(sym, NULL, 0, 0);
        validated_sym = validate_sym_for_reloc(sym);
      }
      /* Preserve legacy behavior: if a symbol exists, emit relocation even if
       * validation failed (e.g. before registration), unless it's a type field. */
      if (sym && !(sym->v & SYM_FIELD))
        reloc_sym = validated_sym ? validated_sym : sym;
    }

    uint32_t imm;
    if (reloc_sym)
    {
      /* For symbol relocations, keep a benign placeholder immediate.
       * Using -4 encodes a self-call (common placeholder) and provides a
       * stable addend independent of any pool flush.
       */
      imm = (uint32_t)-4;
    }
    else
    {
      const int32_t rel = (tag == IROP_TAG_IMM32) ? dest.u.imm32 : addend;
      imm = th_encbranch(ind, ind + rel);
    }

    TRACE("gcall_or_jmp: %d, ind: 0x%x, 0x%x", is_jmp, ind, imm);
    if (imm)
    {
      ot_check(th_bl_t1(imm));
      if (reloc_sym)
      {
        int call_pos = ind - 4; /* th_bl_t1 is always 4 bytes */
        greloc(cur_text_section, reloc_sym, call_pos, R_ARM_THM_JUMP24);
      }
    }
  }
  else
  {
    /* Indirect call through register.
     *
     * When the target type is IROP_BTYPE_FUNC (direct function designator), if the
     * address already lives in a register, clear is_lval so we don't emit a bogus
     * extra load like "ldr ip, [ip]" before blx.
     */
    int bt = irop_get_btype(dest);
    if (bt == IROP_BTYPE_FUNC && dest.is_lval && tag == IROP_TAG_VREG && dest.pr0_reg != PREG_REG_NONE)
    {
      dest.is_lval = 0;
    }

    /* Indirect call/jump: keep argument registers (R0-R3) intact.
     * In particular, for indirect calls the target must NOT live in R0,
     * otherwise arg0 gets overwritten (e.g. fprintfptr(stdout, ...)).
     * Prefer R12/IP which is caller-saved by the ABI.
     */
    if (is_jmp)
    {
      load_to_reg_ir(R_IP, PREG_NONE, dest);
      ot_check(th_bx_reg(R_IP));
    }
    else
    {
      ScratchRegAlloc scratch = get_scratch_reg_with_save((1u << R0) | (1u << R1) | (1u << R2) | (1u << R3));

      /* Keep argument registers off-limits while materializing the target. */
      uint32_t old_exclude = scratch_global_exclude;
      scratch_global_exclude |= (1u << R0) | (1u << R1) | (1u << R2) | (1u << R3);

      load_to_reg_ir(scratch.reg, PREG_NONE, dest);

      scratch_global_exclude = old_exclude;
      ot_check(th_blx_reg(scratch.reg));
      restore_scratch_reg(&scratch);
    }
  }
}

/* IROperand version of load_to_register */
static void load_to_register_ir(int reg, int reg_from, IROperand src)
{
  const char *ctx = "load_to_register_ir";

  /* VT_LOCAL case: check if we need the address or the value */
  if (src.is_local)
  {
    /* Local without lval means we need the ADDRESS - use full load machinery */
    if (!src.is_lval)
    {
      int r1 = (src.pr1_reg != PREG_REG_NONE && irop_is_64bit(src)) ? src.pr1_reg : PREG_REG_NONE;
      load_to_reg_ir(reg, r1, src);
      return;
    }

    /* Local with lval: value is cached in register or needs reload */
    if (src.pr0_reg != PREG_REG_NONE)
    {
      int cached = (reg_from != PREG_NONE) ? reg_from : src.pr0_reg;
      thumb_require_materialized_reg(ctx, "cached local value", cached);
      if (reg != cached)
      {
        ot_check(
            th_mov_reg(reg, cached, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
      }
      return;
    }

    /* Local spilled to stack - reload */
    int r1 = (src.pr1_reg != PREG_REG_NONE && irop_is_64bit(src)) ? src.pr1_reg : PREG_REG_NONE;
    load_to_reg_ir(reg, r1, src);
    return;
  }

  /* If it's an lval or not in a register, do a full load */
  if (src.is_lval || src.pr0_reg == PREG_REG_NONE)
  {
    int r1 = (src.pr1_reg != PREG_REG_NONE && irop_is_64bit(src)) ? src.pr1_reg : PREG_REG_NONE;
    load_to_reg_ir(reg, r1, src);
    return;
  }

  /* Value is in a valid register - move it.
   * For 64-bit values, callers may request moving either the low or high word
   * via 'reg_from'. Using src.pr0 unconditionally breaks word selection. */
  int src_reg = (reg_from != PREG_NONE) ? reg_from : src.pr0_reg;
  thumb_require_materialized_reg(ctx, "source register", src_reg);
  if (reg != src_reg)
  {
    ot_check(
        th_mov_reg(reg, src_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
  }
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
  IROperand lval_op;     /* valid when kind==THUMB_ARG_MOVE_LVAL/STRUCT */
  int struct_word_count; /* valid when kind==THUMB_ARG_MOVE_STRUCT */
} ThumbArgMove;

/* Context for function call generation - reduces parameter passing */
typedef struct CallGenContext
{
  ThumbGenCallSite *call_site;
  TCCAbiCallLayout *layout;
  IROperand *args;
  int argc;
  int stack_size;
} CallGenContext;

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
    IROperand op = m->lval_op;
    /* Use dst_reg_hi for 64-bit types (double, long long) */
    const int hi_reg = (irop_is_64bit(op) && m->dst_reg_hi != PREG_REG_NONE) ? m->dst_reg_hi : PREG_NONE;
    load_to_reg_ir(m->dst_reg, hi_reg, op);
    return;
  }

  if (m->kind == THUMB_ARG_MOVE_STRUCT)
  {
    /* Load struct words into consecutive registers.
     * The lval_op contains the struct address. */
    IROperand op = m->lval_op;
    int word_count = m->struct_word_count;
    int base_dst = m->dst_reg;

    /* Get the struct base address into a scratch register */
    int base_addr_reg = ARM_R12;

    base_addr_reg = get_struct_base_addr(&op, base_addr_reg);

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

/* ========================================================================
 * Helper functions for call argument handling
 * ======================================================================== */

/* Store a word to stack with large offset fallback */
static void store_word_to_stack(int src_reg, int stack_offset)
{
  if (!store_word_to_base(src_reg, ARM_SP, stack_offset, 0))
  {
    /* Offset too large - use alternate scratch register */
    int scratch = (src_reg != ARM_R12) ? ARM_R12 : ARM_LR;
    load_immediate(scratch, stack_offset, NULL, false);
    ot_check(th_str_reg(src_reg, ARM_SP, scratch, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
  }
}

/* Store a word to stack, preserving R0 if needed as scratch */
static void store_word_to_stack_safe(int src_reg, int stack_offset, int base_addr_reg)
{
  if (!store_word_to_base(src_reg, ARM_SP, stack_offset, 0))
  {
    int scratch = (base_addr_reg != ARM_R12) ? ARM_R12 : ARM_R0;
    if (scratch == ARM_R0)
    {
      ot_check(th_push(1 << ARM_R0));
      load_immediate(ARM_R0, stack_offset, NULL, false);
      ot_check(th_str_reg(src_reg, ARM_SP, ARM_R0, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      ot_check(th_pop(1 << ARM_R0));
    }
    else
    {
      load_immediate(scratch, stack_offset, NULL, false);
      ot_check(th_str_reg(src_reg, ARM_SP, scratch, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
  }
}

/* Get struct base address into a register */
static int get_struct_base_addr(const IROperand *arg, int default_reg)
{
  int base_addr_reg = default_reg;

  const int tag = irop_get_tag(*arg);

  if (tag == IROP_TAG_STACKOFF && arg->is_local)
  {
    int local_off = arg->u.imm32;
    if (arg->is_param && local_off >= 0)
      local_off += offset_to_args;

    if (arg->is_llocal)
    {
      int sign = (local_off < 0);
      int abs_off = sign ? -local_off : local_off;
      if (!load_word_from_base(base_addr_reg, ARM_R7, abs_off, sign))
      {
        load_immediate(base_addr_reg, local_off, NULL, false);
        ot_check(th_ldr_reg(base_addr_reg, ARM_R7, base_addr_reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      }
    }
    else
    {
      tcc_machine_addr_of_stack_slot(base_addr_reg, local_off, arg->is_param ? 1 : 0);
    }
  }
  else if (tag == IROP_TAG_SYMREF)
  {
    IRPoolSymref *symref = irop_get_symref_ex(tcc_state->ir, *arg);
    Sym *sym = symref ? symref->sym : NULL;
    int32_t addend = symref ? symref->addend : 0;
    load_immediate(base_addr_reg, (uint32_t)addend, sym, false);
  }
  else if (arg->pr0_reg != PREG_REG_NONE && !arg->pr0_spilled)
  {
    base_addr_reg = arg->pr0_reg;
  }
  else
  {
    IROperand addr_op = *arg;
    addr_op.is_lval = 0;
    load_to_reg_ir(base_addr_reg, PREG_NONE, addr_op);
  }

  return base_addr_reg;
}

/* Build register move for a struct argument */
static int build_reg_move_struct(ThumbArgMove *moves, int move_count, const IROperand *arg, const TCCAbiArgLoc *loc,
                                 int base_reg, ThumbGenCallSite *call_site)
{
  int words = loc->reg_count;
  if (words > 0 && words <= 4)
  {
    moves[move_count++] = (ThumbArgMove){
        .kind = THUMB_ARG_MOVE_STRUCT,
        .dst_reg = base_reg,
        .lval_op = *arg,
        .struct_word_count = words,
    };
  }
  for (int w = 0; w < words && w < loc->reg_count; w++)
    call_site->registers_map |= (1 << (base_reg + w));
  return move_count;
}

/* Build register move for a 64-bit argument */
static int build_reg_move_64bit(ThumbArgMove *moves, int move_count, const IROperand *arg, int base_reg,
                                ThumbGenCallSite *call_site)
{
  if (arg->is_lval)
  {
    moves[move_count++] =
        (ThumbArgMove){.kind = THUMB_ARG_MOVE_LVAL, .dst_reg = base_reg, .dst_reg_hi = base_reg + 1, .lval_op = *arg};
  }
  else if (arg->pr0_reg != PREG_REG_NONE && arg->pr1_reg != PREG_REG_NONE)
  {
    if (arg->pr0_reg != base_reg)
      moves[move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_REG, .dst_reg = base_reg, .src_reg = arg->pr0_reg};
    if (arg->pr1_reg != (base_reg + 1))
      moves[move_count++] =
          (ThumbArgMove){.kind = THUMB_ARG_MOVE_REG, .dst_reg = base_reg + 1, .src_reg = arg->pr1_reg};
  }
  else if (irop_is_immediate(*arg))
  {
    const uint64_t imm64 = (uint64_t)irop_get_imm64_ex(tcc_state->ir, *arg);
    moves[move_count++] =
        (ThumbArgMove){.kind = THUMB_ARG_MOVE_IMM64, .dst_reg = base_reg, .dst_reg_hi = base_reg + 1, .imm64 = imm64};
  }
  else
  {
    moves[move_count++] =
        (ThumbArgMove){.kind = THUMB_ARG_MOVE_LVAL, .dst_reg = base_reg, .dst_reg_hi = base_reg + 1, .lval_op = *arg};
  }

  call_site->registers_map |= (1 << base_reg) | (1 << (base_reg + 1));
  return move_count;
}

/* Build register move for a 32-bit argument */
static int build_reg_move_32bit(ThumbArgMove *moves, int move_count, const IROperand *arg, int base_reg,
                                ThumbGenCallSite *call_site)
{
  if (arg->is_lval)
  {
    moves[move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_LVAL, .dst_reg = base_reg, .lval_op = *arg};
  }
  else if (arg->pr0_reg != PREG_REG_NONE && !arg->pr0_spilled)
  {
    if (arg->pr0_reg != base_reg)
      moves[move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_REG, .dst_reg = base_reg, .src_reg = arg->pr0_reg};
  }
  else if (irop_get_tag(*arg) == IROP_TAG_SYMREF)
  {
    IRPoolSymref *symref = irop_get_symref_ex(tcc_state->ir, *arg);
    Sym *sym = symref ? symref->sym : NULL;
    int32_t addend = symref ? symref->addend : 0;
    moves[move_count++] =
        (ThumbArgMove){.kind = THUMB_ARG_MOVE_IMM, .dst_reg = base_reg, .imm = (uint32_t)addend, .sym = sym};
  }
  else if (irop_get_tag(*arg) == IROP_TAG_IMM32)
  {
    moves[move_count++] =
        (ThumbArgMove){.kind = THUMB_ARG_MOVE_IMM, .dst_reg = base_reg, .imm = (uint32_t)arg->u.imm32, .sym = NULL};
  }
  else if (irop_get_tag(*arg) == IROP_TAG_STACKOFF && arg->is_local && !arg->is_lval)
  {
    moves[move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_LOCAL_ADDR,
                                         .dst_reg = base_reg,
                                         .local_offset = (int)arg->u.imm32,
                                         .local_is_param = arg->is_param ? 1 : 0};
  }
  else
  {
    moves[move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_LVAL, .dst_reg = base_reg, .lval_op = *arg};
  }

  call_site->registers_map |= (1 << base_reg);
  return move_count;
}

/* Place a struct argument on stack */
static void place_stack_arg_struct(const IROperand *arg, const TCCAbiArgLoc *loc, int stack_offset)
{
  int words_in_regs = (loc->kind == TCC_ABI_LOC_REG_STACK) ? loc->reg_count : 0;
  int struct_src_offset = words_in_regs * 4;
  int struct_size = (loc->kind == TCC_ABI_LOC_REG_STACK) ? loc->stack_size : loc->size;
  int words = (struct_size + 3) / 4;

  int base_addr_reg = get_struct_base_addr(arg, ARM_R12);

  for (int w = 0; w < words; ++w)
  {
    int src_off = struct_src_offset + w * 4;
    int dst_off = stack_offset + w * 4;

    /* Load word from struct into LR */
    if (!load_word_from_base(ARM_LR, base_addr_reg, src_off, 0))
    {
      load_immediate(ARM_LR, src_off, NULL, false);
      ot_check(th_ldr_reg(ARM_LR, base_addr_reg, ARM_LR, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }

    store_word_to_stack_safe(ARM_LR, dst_off, base_addr_reg);
  }
}

/* Place a 64-bit argument on stack */
static void place_stack_arg_64bit(const IROperand *arg, int stack_offset)
{
  int lo_offset = stack_offset;
  int hi_offset = stack_offset + 4;

  if (arg->is_lval)
  {
    IROperand op = *arg;
    load_to_reg_ir(ARM_R12, ARM_LR, op);
    store_word_to_stack_safe(ARM_R12, lo_offset, ARM_R12);
    store_word_to_stack_safe(ARM_LR, hi_offset, ARM_R12);
  }
  else if (arg->pr0_reg != PREG_REG_NONE && arg->pr1_reg != PREG_REG_NONE)
  {
    store_word_to_stack(arg->pr0_reg, lo_offset);
    store_word_to_stack(arg->pr1_reg, hi_offset);
  }
  else if (irop_is_immediate(*arg))
  {
    uint64_t imm64 = (uint64_t)irop_get_imm64_ex(tcc_state->ir, *arg);
    load_immediate(ARM_R12, (uint32_t)imm64, NULL, false);
    store_word_to_stack(ARM_R12, lo_offset);
    load_immediate(ARM_R12, (uint32_t)(imm64 >> 32), NULL, false);
    store_word_to_stack(ARM_R12, hi_offset);
  }
  else
  {
    IROperand op = *arg;
    load_to_reg_ir(ARM_R12, ARM_LR, op);
    store_word_to_stack_safe(ARM_R12, lo_offset, ARM_R12);
    store_word_to_stack_safe(ARM_LR, hi_offset, ARM_R12);
  }
}

/* Helper to compute local offset with parameter adjustment */
static int compute_local_offset(const IROperand *arg)
{
  int local_off = (int)arg->u.imm32;
  if (arg->is_param && local_off >= 0)
    local_off += offset_to_args;
  return local_off;
}

/* Place a 32-bit argument on stack */
static void place_stack_arg_32bit(const IROperand *arg, int stack_offset)
{
  if (arg->pr0_reg != PREG_REG_NONE && !arg->pr0_spilled)
  {
    /* Skip R0-R3 sources - handled in pre-shuffle save */
    if (arg->pr0_reg <= ARM_R3)
      return;

    int src_reg = arg->pr0_reg;
    if (arg->is_lval)
    {
      ot_check(th_ldr_imm(ARM_R12, src_reg, 0, 6, ENFORCE_ENCODING_NONE));
      src_reg = ARM_R12;
    }
    store_word_to_stack(src_reg, stack_offset);
  }
  else if (irop_get_tag(*arg) == IROP_TAG_SYMREF)
  {
    IRPoolSymref *symref = irop_get_symref_ex(tcc_state->ir, *arg);
    Sym *sym = symref ? symref->sym : NULL;
    int32_t addend = symref ? symref->addend : 0;
    load_immediate(ARM_R12, (uint32_t)addend, sym, false);
    if (arg->is_lval)
      ot_check(th_ldr_imm(ARM_R12, ARM_R12, 0, 6, ENFORCE_ENCODING_NONE));
    store_word_to_stack(ARM_R12, stack_offset);
  }
  else if (irop_get_tag(*arg) == IROP_TAG_IMM32)
  {
    load_immediate(ARM_R12, (uint32_t)arg->u.imm32, NULL, false);
    if (arg->is_lval)
      ot_check(th_ldr_imm(ARM_R12, ARM_R12, 0, 6, ENFORCE_ENCODING_NONE));
    store_word_to_stack(ARM_R12, stack_offset);
  }
  else if (irop_get_tag(*arg) == IROP_TAG_STACKOFF && arg->is_local && !arg->is_llocal)
  {
    int local_off = compute_local_offset(arg);
    int local_sign = (local_off < 0);
    int local_abs = local_sign ? -local_off : local_off;

    if (arg->is_lval)
    {
      if (!load_word_from_base(ARM_R12, ARM_R7, local_abs, local_sign))
      {
        load_immediate(ARM_R12, local_off, NULL, false);
        ot_check(th_ldr_reg(ARM_R12, ARM_R7, ARM_R12, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      }
    }
    else
    {
      if (!ot(th_add_imm(ARM_R12, ARM_R7, local_off, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)))
      {
        load_immediate(ARM_R12, local_off, NULL, false);
        ot_check(th_add_reg(ARM_R12, ARM_R7, ARM_R12, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE));
      }
    }
    store_word_to_stack(ARM_R12, stack_offset);
  }
  else if (irop_get_tag(*arg) == IROP_TAG_STACKOFF && arg->is_llocal)
  {
    int local_off = compute_local_offset(arg);
    int local_sign = (local_off < 0);
    int local_abs = local_sign ? -local_off : local_off;

    if (!load_word_from_base(ARM_R12, ARM_R7, local_abs, local_sign))
    {
      load_immediate(ARM_R12, local_off, NULL, false);
      ot_check(th_ldr_reg(ARM_R12, ARM_R7, ARM_R12, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    if (arg->is_lval)
      ot_check(th_ldr_imm(ARM_R12, ARM_R12, 0, 6, ENFORCE_ENCODING_NONE));
    store_word_to_stack(ARM_R12, stack_offset);
  }
  else
  {
    IROperand op = *arg;
    load_to_reg_ir(ARM_R12, PREG_NONE, op);
    store_word_to_stack(ARM_R12, stack_offset);
  }
}

/* Build all register argument moves */
static int build_register_arg_moves(CallGenContext *ctx, ThumbArgMove *reg_moves)
{
  int move_count = 0;

  for (int i = 0; i < ctx->argc; ++i)
  {
    const TCCAbiArgLoc *loc = &ctx->layout->locs[i];
    const IROperand *arg = &ctx->args[i];
    const int bt = irop_get_btype(*arg);
    const int is_64bit = irop_is_64bit(*arg);

    if (loc->kind != TCC_ABI_LOC_REG && loc->kind != TCC_ABI_LOC_REG_STACK)
      continue;

    int base_reg = ARM_R0 + loc->reg_base;

    if (bt == IROP_BTYPE_STRUCT)
    {
      move_count = build_reg_move_struct(reg_moves, move_count, arg, loc, base_reg, ctx->call_site);
    }
    else if (is_64bit)
    {
      if (loc->reg_count < 2)
        tcc_error("compiler_error: 64-bit register argument has insufficient registers");
      move_count = build_reg_move_64bit(reg_moves, move_count, arg, base_reg, ctx->call_site);
    }
    else
    {
      move_count = build_reg_move_32bit(reg_moves, move_count, arg, base_reg, ctx->call_site);
    }
  }

  return move_count;
}

/* Pre-save stack arguments that source from R0-R3 before register shuffle */
static void presave_stack_args_from_arg_regs(CallGenContext *ctx)
{
  for (int i = 0; i < ctx->argc; ++i)
  {
    const TCCAbiArgLoc *loc = &ctx->layout->locs[i];
    const IROperand *arg = &ctx->args[i];
    const int bt = irop_get_btype(*arg);

    if (loc->kind == TCC_ABI_LOC_REG)
      continue;
    if (bt == IROP_BTYPE_STRUCT || irop_is_64bit(*arg))
      continue;

    if (arg->pr0_reg != PREG_REG_NONE && !arg->pr0_spilled && arg->pr0_reg <= ARM_R3)
    {
      store_word_to_stack(arg->pr0_reg, loc->stack_off);
    }
  }
}

/* Place all stack arguments */
static void place_stack_arguments(CallGenContext *ctx)
{
  for (int i = 0; i < ctx->argc; ++i)
  {
    const TCCAbiArgLoc *loc = &ctx->layout->locs[i];
    const IROperand *arg = &ctx->args[i];
    const int bt = irop_get_btype(*arg);
    const int is_64bit = irop_is_64bit(*arg);

    if (loc->kind == TCC_ABI_LOC_REG)
      continue;

    int stack_offset = loc->stack_off;

    if (bt == IROP_BTYPE_STRUCT)
      place_stack_arg_struct(arg, loc, stack_offset);
    else if (is_64bit)
      place_stack_arg_64bit(arg, stack_offset);
    else
      place_stack_arg_32bit(arg, stack_offset);
  }
}

/* Handle return value after call */
static void handle_return_value(IROperand dest, int drop_value)
{
  if (drop_value)
    return;

  if (dest.pr0_reg != PREG_REG_NONE && dest.pr0_reg != ARM_R0)
  {
    ot_check(th_mov_reg(dest.pr0_reg, ARM_R0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                        false));
  }

  if (irop_is_64bit(dest) && dest.pr1_reg != PREG_REG_NONE && dest.pr1_reg != ARM_R1)
  {
    ot_check(th_mov_reg(dest.pr1_reg, ARM_R1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                        false));
  }
}

/* ======================================================================== */

ST_FUNC void tcc_gen_machine_func_call_op(SValue *func_target, SValue *call_id_sv, SValue *dest, int drop_value,
                                          TCCIRState *ir, int call_idx)
{
  /* === Validation === */
  if (!call_id_sv || !ir)
    tcc_error("compiler_error: func_call_op requires call_id_sv+ir");

  const IROperand func_target_ir = svalue_to_iroperand(tcc_state->ir, func_target);
  const IROperand dest_ir = svalue_to_iroperand(tcc_state->ir, dest);

  const int call_id = TCCIR_DECODE_CALL_ID(call_id_sv->c.i);
  const int argc_hint = TCCIR_DECODE_CALL_ARGC(call_id_sv->c.i);

  ThumbGenCallSite *call_site = thumb_get_call_site_for_id(call_id);
  if (!call_site)
    tcc_error("compiler_error: no call site found for call_id=%d", call_id);

  /* === Build ABI layout === */
  TCCAbiCallLayout layout;
  memset(&layout, 0, sizeof(layout));

  IROperand *args = NULL;
  const int argc = thumb_build_call_layout_from_ir(ir, call_idx, call_id, argc_hint, &layout, &args);
  if (argc < 0)
    tcc_error("compiler_error: failed to build call layout for call_id=%d", call_id);

  int stack_size = (argc > 0) ? (int)layout.stack_size : 0;

  /* === Setup call context === */
  CallGenContext ctx = {
      .call_site = call_site,
      .layout = &layout,
      .args = args,
      .argc = argc,
      .stack_size = stack_size,
  };

  /* === Preserve nested call registers (R0-R3) === */
  int arg_regs_in_use = call_site->registers_map & 0x0F;
  int arg_regs_push_mask = arg_regs_in_use;
  int arg_regs_push_count = __builtin_popcount((unsigned)arg_regs_push_mask);

  /* AAPCS requires 8-byte SP alignment - pad with R12 if needed */
  if (arg_regs_push_count & 1)
  {
    arg_regs_push_mask |= (1 << ARM_R12);
    arg_regs_push_count++;
  }

  if (arg_regs_push_mask)
  {
    ot_check(th_push((uint16_t)arg_regs_push_mask));
    call_site->used_stack_size += arg_regs_push_count * 4;
  }

  /* === Reserve stack space === */
  stack_size = (stack_size + 7) & ~7; /* 8-byte align */
  if (stack_size > 0)
  {
    gadd_sp(-stack_size);
    call_site->used_stack_size += stack_size;
  }

  /* === Block R0-R3 from scratch allocation during argument setup === */
  uint32_t saved_scratch_exclude = scratch_global_exclude;
  scratch_global_exclude |= 0x0F; /* R0-R3 */

  /* === Build and execute register argument moves === */
  ThumbArgMove reg_moves[8];
  int reg_move_count = build_register_arg_moves(&ctx, reg_moves);

  /* Pre-save stack args sourcing from R0-R3 before register shuffle */
  presave_stack_args_from_arg_regs(&ctx);

  thumb_emit_parallel_arg_moves(reg_moves, reg_move_count);

  /* === Place stack arguments === */
  place_stack_arguments(&ctx);

  /* === Emit call === */
  gcall_or_jump_ir(0, func_target_ir);

  /* Restore scratch register exclusion */
  scratch_global_exclude = saved_scratch_exclude;

  /* === Cleanup === */
  if (stack_size > 0)
  {
    gadd_sp(stack_size);
    call_site->used_stack_size -= stack_size;
  }

  if (arg_regs_push_mask)
  {
    ot_check(th_pop((uint16_t)arg_regs_push_mask));
    call_site->used_stack_size -= arg_regs_push_count * 4;
  }

  handle_return_value(dest_ir, drop_value);

  call_site->registers_map &= ~0x0F; /* Clear R0-R3 */

  if (args)
    tcc_free(args);
  if (layout.locs)
    tcc_free(layout.locs);
}

ST_FUNC void tcc_gen_machine_jump_op(TccIrOp op)
{
  ot_check(th_b_t4(0)); // patch me later
}

ST_FUNC void tcc_gen_machine_conditional_jump_op(IROperand src, TccIrOp op)
{
  int cond = mapcc(src.u.imm32);
  ot_check(th_b_t3(cond, 0)); // patch me later
}

ST_FUNC void tcc_gen_machine_setif_op(IROperand dest, IROperand src, TccIrOp op)
{
  if (dest.pr0_reg >= 15)
    tcc_error("compiler_error: setif_op destination register is invalid (%d)", dest.pr0_reg);
  const int cond = mapcc(src.u.imm32);
  ot_check(th_mov_imm(dest.pr0_reg, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE));
  ot_check(th_it(cond, 0x8)); /* IT <cond> (single instruction) */
  ot_check(th_mov_imm(dest.pr0_reg, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
}

ST_FUNC void tcc_gen_machine_bool_op(IROperand dest, IROperand src1, IROperand src2, TccIrOp op)
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
  const int dest_reg = dest.pr0_reg;
  const int src1_reg = src1.pr0_reg;
  const int src2_reg = src2.pr0_reg;

  if (dest_reg >= 15)
    tcc_error("compiler_error: bool_op destination register is invalid (%d)", dest_reg);

  if (op == TCCIR_OP_BOOL_OR)
  {
    /* ORRS sets flags based on result */
    ot_check(th_orr_reg(dest_reg, src1_reg, src2_reg, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));

    /* If result != 0, dest = 1, else dest = 0. Preserve flags from ORRS. */
    ot_check(th_mov_imm(dest_reg, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE));
    ot_check(th_it(0x1, 0x8)); /* IT NE */
    ot_check(th_mov_imm(dest_reg, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
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
    ot_check(th_cmp_imm(0, src1_reg, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
    ot_check(th_it(0x1, 0x8)); /* IT NE (single instruction) */
    ot_check(th_cmp_imm(0, src2_reg, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
    /* Now flags reflect: NE if both non-zero, EQ if either zero.
     * Materialize without clobbering flags before the conditional move.
     */
    ot_check(th_mov_imm(dest_reg, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE));
    ot_check(th_it(0x1, 0x8)); /* IT NE */
    ot_check(th_mov_imm(dest_reg, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }
}

/* Called at end of each IR instruction to clean up scratch register state.
 * - Restores any pushed scratch registers (POP in reverse push order)
 * - Resets global exclusion mask for next instruction */
ST_FUNC void tcc_gen_machine_end_instruction(void)
{
  restore_all_pushed_scratch_regs();
}

ST_FUNC void tcc_gen_machine_vla_op(IROperand dest, IROperand src1, IROperand src2, TccIrOp op)
{
  switch (op)
  {
  case TCCIR_OP_VLA_ALLOC:
  {
    const char *ctx = "tcc_gen_machine_vla_op";
    /* IR contract: src1=size(bytes), src2=align(bytes), dest unused/NULL. */
    int align = 8;
    if (irop_is_none(src2))
      align = src2.u.imm32;
    if (align < 8)
      align = 8;
    if (align & (align - 1))
      tcc_error("alignment is not a power of 2: %i", align);

    /* Compute new SP in-place in the size register (the size value is dead after this op). */
    int r = src1.pr0_reg;

    if (r != PREG_REG_NONE)
      thumb_require_materialized_reg(ctx, "size", r);

    /* Fallback for non-IR callers: if src1 wasn't allocated to a register (e.g. constant), load to IP. */
    if (r == PREG_NONE || src1.is_const)
    {
      r = R_IP;
      load_to_reg_ir(r, PREG_NONE, src1);
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
    store_ex_ir(R_IP, dest, 0);
    break;
  case TCCIR_OP_VLA_SP_RESTORE:
    /* Restore SP from a fixed stack slot (FP-relative). Use IP as scratch. */
    load_to_reg_ir(R_IP, 0, src1);
    ot_check(th_mov_reg(R_SP, R_IP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
    break;
  default:
    tcc_error("compiler_error: tcc_gen_machine_vla_op unsupported op %d", op);
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

ST_FUNC const char *tcc_get_abi_softcall_name(SValue *src1, SValue *src2, SValue *dest, TccIrOp op)
{
  const int src1_64bit = tcc_is_64bit_operand(src1);
  const int src2_64bit = src2 ? tcc_is_64bit_operand(src2) : 0;
  const int dest_64bit = dest ? tcc_is_64bit_type(dest->type.t) : 0;
  const int src1_size = tcc_get_type_size(&src1->type);
  const int dest_size = dest ? tcc_get_type_size(&dest->type) : 0;

  if (src1_64bit || src2_64bit || dest_64bit)
  {
    switch (op)
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
    default:
      break;
    }
  }
  else
  {
    switch (op)
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
    default:
      break;
    }
  }

  switch (op)
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
    const int is_unsigned = (dest && (dest->type.t & VT_UNSIGNED)) ? 1 : 0;

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
    int cmp_op = src2->c.i;
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
    int is_unsigned = (src1->type.t & VT_UNSIGNED) ? 1 : 0;
    if (is_unsigned)
      return dest_64bit ? "__aeabi_ui2d" : "__aeabi_ui2f";
    return dest_64bit ? "__aeabi_i2d" : "__aeabi_i2f";
  }
  break;
  default:
    break;
  }

  return NULL;
}

ST_FUNC void tcc_gen_machine_func_parameter_op(IROperand src1, IROperand src2, TccIrOp op)
{
  if (irop_is_none(src2))
    tcc_error("compiler_error: func_parameter_op requires src2");

  /* Decode call_id and parameter index from src2.
   * NOTE: src2 may be represented either as inline IMM32 or as an I64 pool entry
   * (e.g. when the packed value doesn't fit signed int32). Always decode from the
   * raw low 32 bits to preserve the bit-packing contract.
   */
  const uint32_t encoded = (uint32_t)irop_get_imm64_ex(tcc_state->ir, src2);
  int call_id = TCCIR_DECODE_CALL_ID(encoded);
  int param_index = TCCIR_DECODE_PARAM_IDX(encoded);

  /* Find or create call site for this call_id */
  ThumbGenCallSite *call_site = thumb_get_or_create_call_site(call_id);
  if (call_site == NULL)
  {
    tcc_error("compiler_error: failed to allocate call site for call_id=%d", call_id);
    return;
  }

  /* FUNCPARAMVOID is a marker for a 0-argument call.
   * Ensure the call site exists, but do not create a fake argument entry. */
  if (op == TCCIR_OP_FUNCPARAMVOID)
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
