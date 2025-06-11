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

#include "arch/arm/arm.h"
#include "arch/arm/ssa_opt_arm.h"
#include "arm-thumb-defs.h"
#include "ir/opt.h"
#include "tcc-chained-hash.h"
#include "tcc.h"
#include "tccir.h"
#include "tccls.h"
#include "tcctype.h"

static void load_full_const(int r, int r1, uint32_t imm_lo, uint32_t imm_hi);

/* Workaround for TCC ARM ABI bugs:
 * 1. int64_t args miscount register pairs  2. 5th+ args not correctly pushed to stack
 * By passing sym through a file-scope global, load_full_const stays at 4 register args.
 * Set _lfc_sym before calling load_full_const; it is consumed and reset to NULL inside. */
static struct Sym *_lfc_sym;

/* Helper macro: split a 64-bit value into (lo, hi) uint32_t pair for load_full_const.
 * Avoids int64_t in function signatures — TCC ARM codegen miscounts int64_t register pairs. */
#define LFC_SPLIT(v) (uint32_t)((uint64_t)(v)), (uint32_t)((uint64_t)(v) >> 32)

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

#include <stdio.h>
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
#include "arch/fpu/arm/fpv5-d16.h"
#include "arch/arm/thumb/thumb.h"
#include "arch/arm/thumb/thop_adr.h"
#include "arch/arm/thumb/thop_alu_imm.h"
#include "arch/arm/thumb/thop_alu_reg.h"
#include "arch/arm/thumb/thop_block.h"
#include "arch/arm/thumb/thop_branch.h"
#include "arch/arm/thumb/thop_cmp.h"
#include "arch/arm/thumb/thop_extend.h"
#include "arch/arm/thumb/thop_ldr_literal.h"
#include "arch/arm/thumb/thop_ldrd.h"
#include "arch/arm/thumb/thop_mem_imm.h"
#include "arch/arm/thumb/thop_mem_reg.h"
#include "arch/arm/thumb/thop_mov.h"
#include "arch/arm/thumb/thop_mul.h"
#include "arch/arm/thumb/thop_mvn.h"
#include "arch/arm/thumb/thop_pld.h"
#include "arch/arm/thumb/thop_shift_imm.h"
#include "arch/arm/thumb/thop_shift_reg.h"
#include "arch/arm/thumb/thop_system.h"

#include <inttypes.h>

int load_word_from_base(int ir, int base, int fc, int sign);

static inline thumb_flags_behaviour flags_safe(void)
{
  if (tcc_state->ir && tcc_state->ir->codegen_flags_live)
    return FLAGS_BEHAVIOUR_BLOCK;
  return FLAGS_BEHAVIOUR_NOT_IMPORTANT;
}

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

ST_DATA const char *const target_machine_defs = "__arm__\0"
                                                "__arm\0"
                                                "arm\0"
                                                "__arm_elf__\0"
                                                "__arm_elf\0"
                                                "arm_elf\0"
#if defined TCC_TARGET_ARM_ARCHV8M
                                                "__ARM_ARCH_8M__\0"
                                                "__ARM_ARCH_EXT_IDIV__\0"
                                                "__thumb__\0"
#endif // TCC_TARGET_ARM_ARCHV8M
                                                "__VFP_FP__\0"
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
unsigned char allow_r9_write;
unsigned char pic;

int offset_to_args = 0;

thumb_flags_behaviour g_setflags = FLAGS_BEHAVIOUR_SET;

uint32_t caller_saved_registers;
uint32_t pushed_registers;
int allocated_stack_size;
int epilogue_stack_dealloc;     /* total SUB SP amount to restore in epilogue (includes alignment pad) */
int callee_push_size = 0;       /* bytes pushed BELOW FP in two-phase push */
uint32_t callee_saved_regs = 0; /* register mask for second push (below FP) */
int vararg_push_size = 0;       /* bytes pushed for variadic r0-r3 save (16 or 0) */

/* Adjust a local/spill frame offset.
 *
 * When FP is used with two-phase push: adjusts by callee_push_size (regs
 * pushed below FP).
 *
 * When FP is omitted: converts FP-relative negative offsets to SP-relative
 * positive offsets.  The alignment pad sits at the top of the SUB SP region
 * (right below pushed regs), so locals are addressed relative to
 * allocated_stack_size (without pad):
 * FP + frame_offset = SP + allocated_stack_size + frame_offset. */
static inline int fp_adjust_local_offset(int frame_offset, int is_param)
{
  if (is_param)
    return frame_offset;

  if (!tcc_state->need_frame_pointer && frame_offset <= 0)
  {
    /* Convert FP-relative (negative) to SP-relative (positive).
     * FP + frame_offset = SP + allocated_stack_size + frame_offset. */
    return allocated_stack_size + frame_offset;
  }

  if (frame_offset < 0 && callee_push_size > 0)
    return frame_offset - callee_push_size;

  return frame_offset;
}

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
static int scratch_push_type[128]; /* 1 = PUSH, 2 = STR to scratch area */
static int scratch_push_count = 0;

/* Flag: set to 1 when a real-run (non-dry-run) scratch PUSH is emitted.
 * Used by codegen to detect when FP omission caused SP-corrupting pushes
 * and trigger recompilation with FP enabled. */
static int real_run_scratch_push_detected = 0;

/* Tail-call flag: when set, the next gcall_or_jump_mop emits B (branch)
 * instead of BL (branch-with-link), and post-call cleanup is skipped. */
static int tail_call_pending = 0;

/* Current slot index within the scratch save area (0-based).
 * Incremented on save, decremented on restore. */
static int scratch_save_slot = 0;

/* Debug tracking: current IR opcode being processed (set by codegen.c) */
int g_debug_current_op = -1;

int is_valid_opcode(thumb_opcode op);
int ot(thumb_opcode op);
int ot_check(thumb_opcode op);
static int ot_check_mov_reg(uint32_t rd, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                            thumb_enforce_encoding enc, bool in_it);
static int ot_check_ldr_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc);
static int ot_check_str_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc);
static void mov_equiv_reset_all(void);
static void imm_cache_reset_all(void);
static void imm_cache_invalidate_reg(int reg);
ST_FUNC void tcc_gen_machine_strldr_cache_reset(void);
ST_FUNC void tcc_gen_machine_imm_cache_reset(void);
static void thumb_require_materialized_reg(const char *ctx, const char *operand, int reg);
static bool thumb_is_hw_reg(int reg);
static int get_struct_base_addr_mop(const MachineOperand *mop, int default_reg);
static int find_call_scratch(uint32_t extra_exclude, uint32_t arg_move_dst_mask);
int th_has_immediate_value(int r);
int load_word_from_base(int ir, int base, int fc, int sign);
int th_patch_call(int t, int a);
/* Structure to track scratch register allocation with potential save/restore */
typedef struct ScratchRegAlloc
{
  int reg : 29;            /* The allocated scratch register (range 0-15 for ARM) */
  uint32_t saved : 2;      /* 0=not saved, 1=PUSH to stack, 2=STR to scratch area */
  uint32_t would_save : 1; /* Whether a push was needed (set in both dry-run and real emit) */
} ScratchRegAlloc;

/* Forward declarations needed by multi-scratch helpers. */
static ScratchRegAlloc get_scratch_reg_with_save(uint32_t exclude_regs);
static void restore_scratch_reg(ScratchRegAlloc *alloc);
static void load_from_base(int r, int r1, int irop_btype, int is_unsigned, int fc, int sign, uint32_t base);
static void th_store32_imm_or_reg_ex(int src_reg, uint32_t base_reg, int abs_off, int sign, uint32_t extra_exclude);

/* Resolve the base register for a captured variable access.
 * For depth 1, returns R10 directly.
 * For depth > 1, emits LDR chain to follow ancestor frame pointers
 * and returns a scratch register holding the target ancestor's FP.
 * Caller must restore scratch via *out_scratch when done. */
static int resolve_chain_base(TCCIRState *ir, int ci, uint32_t exclude_regs, ScratchRegAlloc *out_scratch,
                              int *used_scratch)
{
  int depth = ir->captured_chain_depths[ci];
  if (depth <= 1)
  {
    *used_scratch = 0;
    return architecture_config.static_chain_reg; /* R10 */
  }

  /* Multi-hop: follow chain through (depth - 1) intermediate frames.
   * Each frame saves its incoming R10 at [FP - 4] (CHAIN_SLOT_OFFSET). */
  *out_scratch = get_scratch_reg_with_save(exclude_regs);
  *used_scratch = 1;

  /* Start from R10 (points to immediate parent's FP) */
  thumb_shift no_shift = {THUMB_SHIFT_NONE, 0, THUMB_SHIFT_IMMEDIATE};
  ot_check_mov_reg(out_scratch->reg, architecture_config.static_chain_reg, flags_safe(), no_shift,
                   ENFORCE_ENCODING_NONE, false);

  for (int hop = 1; hop < depth; hop++)
  {
    /* LDR temp, [temp, #-4]  — follow chain link */
    load_from_base(out_scratch->reg, PREG_REG_NONE, IROP_BTYPE_INT32, 0, 4 /* abs */, 1 /* sign: negative */,
                   out_scratch->reg);
  }
  return out_scratch->reg;
}

typedef struct ScratchRegAllocs
{
  int regs[8];         /* The allocated scratch registers */
  int count;           /* Number of registers allocated */
  uint32_t saved_mask; /* Bitmask of registers that were saved (pushed) */
} ScratchRegAllocs;

/* ============================================================
 * MachineCodegenContext — per-instruction scratch-register tracker
 * ============================================================
 * Used by the MachineOperand-based (_mop) code-generation path.
 * Callers allocate scratches via mach_alloc_scratch(), then call
 * mach_release_all() at the end of the instruction to pop them in LIFO order.
 */

/* Forward declarations needed by the mach_* helpers (defined later in this file). */
typedef thumb_opcode (*thumb_imm_handler_t)(uint32_t rd, uint32_t rn, uint32_t imm,
                                            thumb_flags_behaviour flags_behaviour,
                                            thumb_enforce_encoding enforce_encoding);

/* Dispatch an imm_handler call through a direct call instead of an indirect
 * (function pointer) call.  Same workaround as thumb_call_reg_handler: the
 * cross-compiler miscompiles indirect calls that combine an sret return
 * (thumb_opcode is 8 bytes) with stack-passed arguments — the callee reads
 * garbage for the 5th/6th parameters (flags/enc), so e.g. the high-half SBCS
 * of a 64-bit CMP silently loses its S bit.  Comparing the pointer and
 * branching to a direct call makes the cross emit correct argument passing. */
static thumb_opcode thumb_call_imm_handler(thumb_imm_handler_t fn, uint32_t rd, uint32_t rn, uint32_t imm,
                                           thumb_flags_behaviour flags, thumb_enforce_encoding encoding)
{
  if (fn == th_add_imm)
    return th_add_imm(rd, rn, imm, flags, encoding);
  if (fn == th_sub_imm)
    return th_sub_imm(rd, rn, imm, flags, encoding);
  if (fn == th_adc_imm)
    return th_adc_imm(rd, rn, imm, flags, encoding);
  if (fn == th_sbc_imm)
    return th_sbc_imm(rd, rn, imm, flags, encoding);
  if (fn == th_cmp_imm_handler)
    return th_cmp_imm_handler(rd, rn, imm, flags, encoding);
  if (fn == th_lsl_imm)
    return th_lsl_imm(rd, rn, imm, flags, encoding);
  if (fn == th_lsr_imm)
    return th_lsr_imm(rd, rn, imm, flags, encoding);
  if (fn == th_asr_imm)
    return th_asr_imm(rd, rn, imm, flags, encoding);
  if (fn == th_ror_imm)
    return th_ror_imm(rd, rn, imm, flags, encoding);
  if (fn == th_orr_imm)
    return th_orr_imm(rd, rn, imm, flags, encoding);
  if (fn == th_and_imm)
    return th_and_imm(rd, rn, imm, flags, encoding);
  if (fn == th_eor_imm)
    return th_eor_imm(rd, rn, imm, flags, encoding);
  if (fn == th_bic_imm)
    return th_bic_imm(rd, rn, imm, flags, encoding);
  if (fn == th_orn_imm)
    return th_orn_imm(rd, rn, imm, flags, encoding);
  /* Unreachable for known handlers — fallback to direct call. */
  return fn(rd, rn, imm, flags, encoding);
}
int store_word_to_base(int ir, int base, int fc, int sign);
static ScratchRegAlloc th_offset_to_reg_ex(int off, int sign, uint32_t exclude_regs);

#define MACH_CTX_MAX_SCRATCH 12

typedef struct MachineCodegenContext
{
  ScratchRegAlloc scratches[MACH_CTX_MAX_SCRATCH];
  int n_scratch;
} MachineCodegenContext;

/* Phase-3 per-instruction scratch constraint counters.
 * Incremented/set by mach_alloc_scratch(); reset and read via the
 * tcc_gen_machine_insn_scratch_*() public functions.
 * Declared here (before mach_alloc_scratch) to avoid a forward-reference to
 * dry_run_state which is defined later in the file. */
static int g_insn_scratch_allocs = 0;     /* total scratch allocs this instruction */
static uint16_t g_insn_scratch_saves = 0; /* registers that required PUSH this instruction */


/* Allocate a scratch register for the current instruction.
 * excl: bitmask of registers that must not be chosen.
 * The allocation is recorded in ctx so mach_release_all() can free it. */
static int mach_alloc_scratch(MachineCodegenContext *ctx, uint32_t excl)
{
  if (ctx->n_scratch >= MACH_CTX_MAX_SCRATCH)
    tcc_error("compiler_error: mach_alloc_scratch: per-instruction scratch limit exceeded");
  ScratchRegAlloc alloc = get_scratch_reg_with_save(excl);
  ctx->scratches[ctx->n_scratch++] = alloc;
  /* Phase-3 constraint recording: track count and save-mask per instruction.
   * Reset with tcc_gen_machine_insn_scratch_reset() before each dispatch call;
   * read back with the tcc_gen_machine_insn_scratch_*() accessors after it. */
  g_insn_scratch_allocs++;
  if (alloc.would_save)
    g_insn_scratch_saves |= (uint16_t)(1u << (unsigned)alloc.reg);
  return alloc.reg;
}

/* Release all scratch registers allocated for the current instruction in
 * reverse (LIFO) order — required because ARM push/pop works by register
 * number, so the last-pushed register must be popped first. */
static void mach_release_all(MachineCodegenContext *ctx)
{
  for (int i = ctx->n_scratch - 1; i >= 0; i--)
    restore_scratch_reg(&ctx->scratches[i]);
  ctx->n_scratch = 0;
}

/* Ensure a MachineOperand is in a physical register and return that register.
 *
 * For MACH_OP_REG without needs_deref: returns the register directly (no code).
 * For all other kinds (SPILL, IMM, FRAME_ADDR, SYMBOL, PARAM_STACK) or
 * MACH_OP_REG with needs_deref: allocates a scratch register, emits the
 * necessary load instructions, and returns the scratch register.
 *
 * excl: bitmask of registers that must not be used for any scratch. */
static int mach_ensure_in_reg(MachineCodegenContext *ctx, const MachineOperand *op, uint32_t excl)
{
  switch (op->kind)
  {
  case MACH_OP_NONE:
    /* Unresolved operand: vreg has no register allocation (dead path,
     * uninitialized variable, etc.).  Return a scratch register loaded
     * with zero — the value is undefined but we must not crash. */
    {
      int r = mach_alloc_scratch(ctx, excl);
      tcc_machine_load_constant(r, PREG_REG_NONE, 0, 0, NULL);
      return r;
    }

  case MACH_OP_REG:
    if (!op->needs_deref)
      return op->u.reg.r0;
    {
      /* Register-indirect: op->u.reg.r0 is an address; load the value. */
      int r = mach_alloc_scratch(ctx, excl | (1u << (uint32_t)op->u.reg.r0));
      load_from_base(r, PREG_REG_NONE, op->btype, (int)op->is_unsigned, 0, 0, (uint32_t)op->u.reg.r0);
      return r;
    }

  case MACH_OP_SPILL:
    if (!op->needs_deref)
    {
      /* Simple spill: load the word-sized register value from the spill slot. */
      int r = mach_alloc_scratch(ctx, excl);
      tcc_machine_load_spill_slot(r, op->u.spill.offset);
      return r;
    }
    else
    {
      /* Double indirection (VT_LLOCAL): the spill slot holds a pointer.
       * Step 1: load the pointer from the spill slot.
       * Step 2: dereference the pointer to get the actual value. */
      int ptr_r = mach_alloc_scratch(ctx, excl);
      tcc_machine_load_spill_slot(ptr_r, op->u.spill.offset);
      int val_r = mach_alloc_scratch(ctx, excl | (1u << (uint32_t)ptr_r));
      load_from_base(val_r, PREG_REG_NONE, op->btype, (int)op->is_unsigned, 0, 0, (uint32_t)ptr_r);
      return val_r;
    }

  case MACH_OP_IMM:
  {
    int r = mach_alloc_scratch(ctx, excl);
    tcc_machine_load_constant(r, PREG_REG_NONE, op->u.imm.val, 0, NULL);
    return r;
  }

  case MACH_OP_FRAME_ADDR:
  {
    /* Compute the address FP + offset (address-of a local variable). */
    int r = mach_alloc_scratch(ctx, excl);
    tcc_machine_addr_of_stack_slot(r, op->u.frame.offset, 0 /* not param */);
    return r;
  }

  case MACH_OP_SYMBOL:
  {
    Sym *raw_sym = op->u.sym.sym;
    Sym *sym = raw_sym ? validate_sym_for_reloc(raw_sym) : NULL;
    if (!op->needs_deref)
    {
      /* Load symbol address (with addend baked in). */
      int r = mach_alloc_scratch(ctx, excl);
      tcc_machine_load_constant(r, PREG_REG_NONE, op->u.sym.addend, 0, sym);
      return r;
    }
    else
    {
      /* Load symbol address into a scratch base reg, then dereference. */
      int r = mach_alloc_scratch(ctx, excl);
      int base = mach_alloc_scratch(ctx, excl | (1u << (uint32_t)r));
      tcc_machine_load_constant(base, PREG_REG_NONE, 0, 0, sym);
      const int32_t addend = op->u.sym.addend;
      load_from_base(r, PREG_REG_NONE, op->btype, (int)op->is_unsigned, addend < 0 ? (int)(-addend) : (int)addend,
                     addend < 0 ? 1 : 0, (uint32_t)base);
      return r;
    }
  }

  case MACH_OP_PARAM_STACK:
  {
    /* Stack-passed parameter: always load the value from the caller's argument
     * frame.  NOTE: needs_deref may be false here (cleared by mach_resolve_deref_64
     * for 64-bit split operands), but the load is still required — needs_deref=false
     * in this context means "not a pointer-to-follow", not "compute address". */
    int r = mach_alloc_scratch(ctx, excl);
    const int adjusted = op->u.param.offset + offset_to_args;
    const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
    const int sign = (adjusted < 0);
    const int abs_off = sign ? -adjusted : adjusted;
    load_from_base(r, PREG_REG_NONE, op->btype, (int)op->is_unsigned, abs_off, sign, (uint32_t)base_reg);
    return r;
  }

  case MACH_OP_CHAIN_REL:
  {
    /* Captured variable: load from parent frame via static chain. */
    ScratchRegAlloc chain_scratch = {0};
    int chain_used = 0;
    int base = resolve_chain_base(tcc_state->ir, op->u.chain.chain_index, excl, &chain_scratch, &chain_used);
    int r = mach_alloc_scratch(ctx, excl | (1u << (uint32_t)base));
    int32_t off = op->u.chain.offset;
    int sign = (off < 0);
    int abs_off = sign ? (int)(-off) : (int)off;
    load_from_base(r, PREG_REG_NONE, op->btype, (int)op->is_unsigned, abs_off, sign, (uint32_t)base);
    if (chain_used)
      restore_scratch_reg(&chain_scratch);
    return r;
  }

  default:
    tcc_error("compiler_error: mach_ensure_in_reg: unhandled kind %d", (int)op->kind);
    return PREG_REG_NONE;
  }
}

/* Try to emit an immediate-form instruction for src2; if the encoding succeeds,
 * sets *imm_emitted=true and returns PREG_REG_NONE.  Otherwise loads src2 into
 * a scratch register and returns it (like mach_ensure_in_reg). */
static int mach_ensure_imm_or_reg(MachineCodegenContext *ctx, const MachineOperand *op, uint32_t excl,
                                  thumb_imm_handler_t imm_handler, int dest_reg, int src1_reg,
                                  thumb_flags_behaviour flags, bool *imm_emitted)
{
  *imm_emitted = false;
  if (op->kind == MACH_OP_IMM && imm_handler)
  {
    const uint32_t imm_val = (uint32_t)op->u.imm.val;
    if (ot(thumb_call_imm_handler(imm_handler, (uint32_t)dest_reg, (uint32_t)src1_reg, imm_val, flags,
                                  ENFORCE_ENCODING_NONE)))
    {
      *imm_emitted = true;
      return PREG_REG_NONE;
    }
  }
  return mach_ensure_in_reg(ctx, op, excl);
}

/* Determine (or allocate) the destination register for the current instruction.
 * Returns the physical register that should hold the result.
 * If the destination is a spill slot or needs pointer write-back, allocates a
 * scratch; call mach_writeback_dest() after emitting the instruction. */
static int mach_get_dest_reg(MachineCodegenContext *ctx, const MachineOperand *op, uint32_t excl)
{
  if (!op || op->kind == MACH_OP_NONE)
    return R0; /* CMP / flag-setting ops: Rd is ignored. */

  switch (op->kind)
  {
  case MACH_OP_REG:
    if (!op->needs_deref && op->u.reg.r0 != (int)PREG_REG_NONE)
      return op->u.reg.r0;
    /* No pre-allocated register or store-through-pointer: need scratch. */
    return mach_alloc_scratch(ctx, excl);

  case MACH_OP_SPILL:
  case MACH_OP_FRAME_ADDR:
  case MACH_OP_PARAM_STACK:
  case MACH_OP_CHAIN_REL:
  case MACH_OP_SYMBOL:
    return mach_alloc_scratch(ctx, excl);

  default:
    tcc_error("compiler_error: mach_get_dest_reg: unexpected kind %d", (int)op->kind);
    return PREG_REG_NONE;
  }
}

/* Store the result in 'reg' back to the destination described by *op.
 * Only needed when the destination was a spill slot, stack parameter, or an
 * lvalue (store-through-pointer). Must be called after mach_get_dest_reg()
 * allocated a scratch for those cases. */
static void mach_writeback_dest(const MachineOperand *op, int reg)
{
  if (!op || op->kind == MACH_OP_NONE)
    return;

  switch (op->kind)
  {
  case MACH_OP_REG:
    if (!op->needs_deref)
    {
      if (reg != op->u.reg.r0 && op->u.reg.r0 != (int)PREG_REG_NONE)
        ot_check_mov_reg((uint32_t)op->u.reg.r0, (uint32_t)reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                         ENFORCE_ENCODING_NONE, false);
    }
    else
    {
      /* Store through pointer. */
      if (!store_word_to_base(reg, op->u.reg.r0, 0, 0))
      {
        uint32_t excl = (1u << (uint32_t)reg) | (1u << (uint32_t)op->u.reg.r0);
        ScratchRegAlloc rr = get_scratch_reg_with_save(excl);
        ot_check(th_str_reg((uint32_t)reg, (uint32_t)op->u.reg.r0, (uint32_t)rr.reg, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&rr);
      }
    }
    break;

  case MACH_OP_SPILL:
    tcc_machine_store_spill_slot(reg, op->u.spill.offset);
    break;

  case MACH_OP_FRAME_ADDR:
    /* Local stack slot address used as an lvalue destination.  Write the
     * result back to the underlying frame slot. */
    tcc_machine_store_spill_slot(reg, op->u.frame.offset);
    break;

  case MACH_OP_PARAM_STACK:
    tcc_machine_store_param_slot(reg, op->u.param.offset);
    break;

  case MACH_OP_CHAIN_REL:
  {
    /* Captured variable: store to parent frame via static chain. */
    ScratchRegAlloc chain_scratch = {0};
    int chain_used = 0;
    uint32_t excl = (1u << (uint32_t)reg);
    int base = resolve_chain_base(tcc_state->ir, op->u.chain.chain_index, excl, &chain_scratch, &chain_used);
    int32_t off = op->u.chain.offset;
    int sign = (off < 0);
    int abs_off = sign ? (int)(-off) : (int)off;
    if (!store_word_to_base(reg, base, abs_off, sign))
    {
      ScratchRegAlloc rr = th_offset_to_reg_ex(abs_off, sign, excl | (1u << (uint32_t)base));
      ot_check(th_str_reg((uint32_t)reg, (uint32_t)base, (uint32_t)rr.reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      restore_scratch_reg(&rr);
    }
    if (chain_used)
      restore_scratch_reg(&chain_scratch);
    break;
  }

  case MACH_OP_SYMBOL:
  {
    /* Global variable: load symbol address, then store through it. */
    Sym *sym = op->u.sym.sym ? validate_sym_for_reloc(op->u.sym.sym) : NULL;
    uint32_t excl = (1u << (uint32_t)reg);
    ScratchRegAlloc rr = get_scratch_reg_with_save(excl);
    tcc_machine_load_constant(rr.reg, PREG_REG_NONE, 0, 0, sym);
    const int32_t addend = op->u.sym.addend;
    const int abs_off = addend < 0 ? (int)(-addend) : (int)addend;
    const int sign = addend < 0 ? 1 : 0;
    if (!store_word_to_base(reg, rr.reg, abs_off, sign))
    {
      ScratchRegAlloc rr2 = th_offset_to_reg_ex(abs_off, sign, excl | (1u << (uint32_t)rr.reg));
      ot_check(
          th_str_reg((uint32_t)reg, (uint32_t)rr.reg, (uint32_t)rr2.reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      restore_scratch_reg(&rr2);
    }
    restore_scratch_reg(&rr);
    break;
  }

  default:
    tcc_error("compiler_error: mach_writeback_dest: unexpected kind %d", (int)op->kind);
  }
}

/* Public wrappers for inline asm codegen (arm-thumb-asm.c). These
 * materialise a MachineOperand into/from a specific physical register,
 * managing scratch allocation internally.
 *
 * tcc_gen_mach_load_to_reg loads directly into dest_reg whenever possible
 * (no scratch intermediary) to avoid clobbering other live registers —
 * critical when asm_gen_code loads multiple operands sequentially. */
void tcc_gen_mach_load_to_reg(int dest_reg, const MachineOperand *op)
{
  switch (op->kind)
  {
  case MACH_OP_REG:
    if (!op->needs_deref)
    {
      if (op->u.reg.r0 != dest_reg)
        ot_check_mov_reg((uint32_t)dest_reg, (uint32_t)op->u.reg.r0, flags_safe(), THUMB_SHIFT_DEFAULT,
                         ENFORCE_ENCODING_NONE, false);
      return;
    }
    /* Register-indirect: r0 is an address, load [r0] into dest_reg. */
    load_from_base(dest_reg, PREG_REG_NONE, op->btype, (int)op->is_unsigned, 0, 0, (uint32_t)op->u.reg.r0);
    return;

  case MACH_OP_SPILL:
    if (!op->needs_deref)
    {
      tcc_machine_load_spill_slot(dest_reg, op->u.spill.offset);
      return;
    }
    else
    {
      /* Double indirection (VT_LLOCAL): spill slot holds a pointer.
       * Load pointer into dest_reg, then dereference into dest_reg. */
      tcc_machine_load_spill_slot(dest_reg, op->u.spill.offset);
      load_from_base(dest_reg, PREG_REG_NONE, op->btype, (int)op->is_unsigned, 0, 0, (uint32_t)dest_reg);
      return;
    }

  case MACH_OP_IMM:
    tcc_machine_load_constant(dest_reg, PREG_REG_NONE, op->u.imm.val, 0, NULL);
    return;

  case MACH_OP_FRAME_ADDR:
    tcc_machine_addr_of_stack_slot(dest_reg, op->u.frame.offset, 0);
    return;

  case MACH_OP_SYMBOL:
  {
    Sym *sym = op->u.sym.sym ? validate_sym_for_reloc(op->u.sym.sym) : NULL;
    if (!op->needs_deref)
    {
      tcc_machine_load_constant(dest_reg, PREG_REG_NONE, op->u.sym.addend, 0, sym);
      return;
    }
    /* Symbol deref: load address into dest_reg as scratch, then dereference.
     * Use get_scratch_reg_with_save for the base so it won't clobber dest_reg. */
    {
      uint32_t excl = (1u << (uint32_t)dest_reg);
      ScratchRegAlloc base_alloc = get_scratch_reg_with_save(excl);
      tcc_machine_load_constant(base_alloc.reg, PREG_REG_NONE, 0, 0, sym);
      const int32_t addend = op->u.sym.addend;
      load_from_base(dest_reg, PREG_REG_NONE, op->btype, (int)op->is_unsigned,
                     addend < 0 ? (int)(-addend) : (int)addend, addend < 0 ? 1 : 0, (uint32_t)base_alloc.reg);
      restore_scratch_reg(&base_alloc);
    }
    return;
  }

  case MACH_OP_PARAM_STACK:
  {
    const int adjusted = op->u.param.offset + offset_to_args;
    const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
    const int sign = (adjusted < 0);
    const int abs_off = sign ? -adjusted : adjusted;
    load_from_base(dest_reg, PREG_REG_NONE, op->btype, (int)op->is_unsigned, abs_off, sign, (uint32_t)base_reg);
    return;
  }

  case MACH_OP_CHAIN_REL:
  {
    ScratchRegAlloc chain_scratch = {0};
    int chain_used = 0;
    uint32_t excl = (1u << (uint32_t)dest_reg);
    int base = resolve_chain_base(tcc_state->ir, op->u.chain.chain_index, excl, &chain_scratch, &chain_used);
    int32_t off = op->u.chain.offset;
    int sign = (off < 0);
    int abs_off = sign ? (int)(-off) : (int)off;
    load_from_base(dest_reg, PREG_REG_NONE, op->btype, (int)op->is_unsigned, abs_off, sign, (uint32_t)base);
    if (chain_used)
      restore_scratch_reg(&chain_scratch);
    return;
  }

  default:
  {
    /* Fallback: use scratch + mov for anything unexpected. */
    MachineCodegenContext ctx = {{}, 0};
    int r = mach_ensure_in_reg(&ctx, op, (1u << (uint32_t)dest_reg));
    if (r != dest_reg)
      ot_check_mov_reg((uint32_t)dest_reg, (uint32_t)r, flags_safe(), THUMB_SHIFT_DEFAULT,
                       ENFORCE_ENCODING_NONE, false);
    mach_release_all(&ctx);
    return;
  }
  }
}

void tcc_gen_mach_store_from_reg(int src_reg, const MachineOperand *op)
{
  mach_writeback_dest(op, src_reg);
}

/* ============================================================
 * Dry-Run Code Generation State
 * ============================================================
 * Two-pass code generation system for optimal register allocation.
 * Pass 1 (Dry Run): Analyze register needs without emitting code
 * Pass 2 (Real Emit): Generate code with optimal prologue based on Pass 1
 */

typedef struct CodeGenDryRunState
{
  int active;                   /* 1 = dry run, 0 = real emit */
  uint32_t scratch_regs_pushed; /* Bitmap of regs pushed as scratch */
  int scratch_push_count;       /* Total scratch push operations */
  int lr_push_count;            /* Times LR specifically was pushed */
  int instruction_count;        /* IR instructions processed */
} CodeGenDryRunState;

static CodeGenDryRunState dry_run_state;

/* Separate literal pool for dry-run mode to avoid modifying the real pool.
 * This allows accurate code size tracking without affecting the real pass. */
static ThumbLiteralPoolEntry *dry_run_literal_pool = NULL;
static int dry_run_literal_pool_count = 0;
static int dry_run_literal_pool_size = 0;

/* Literal pool dedup uses the same bucket+chain scheme as TinyCC's ELF hashes.
 * We only hash entries created through th_literal_pool_find_or_allocate(), so
 * plain th_literal_pool_allocate() users stay distinct. */
#define LITERAL_POOL_HASH_BUCKET_COUNT 512
#define LITERAL_POOL_LOOKUP_CACHE_SIZE 16

typedef struct LiteralPoolLookupCacheEntry
{
  Sym *sym;
  int64_t imm;
  int pool_index;
  uint32_t hash;
  int valid;
} LiteralPoolLookupCacheEntry;

typedef struct LiteralPoolLookupCache
{
  LiteralPoolLookupCacheEntry entries[LITERAL_POOL_LOOKUP_CACHE_SIZE];
} LiteralPoolLookupCache;

static TCCChainedHash literal_pool_hash;
static LiteralPoolLookupCache literal_pool_last_lookup;

static inline uint32_t literal_pool_hash_func(Sym *sym, int64_t imm)
{
  /* 32-bit hash to avoid expensive 64-bit multiply on Cortex-M */
  uint32_t h = (uint32_t)(uintptr_t)sym;
  h ^= (uint32_t)imm;
  h ^= (uint32_t)((uint64_t)imm >> 32);
  h ^= h >> 16;
  h *= 0x45d9f3bU;
  h ^= h >> 16;
  return h;
}

static void literal_pool_hash_clear(TCCChainedHash *hash)
{
  tcc_chained_hash_clear(hash);
}

static void literal_pool_lookup_cache_clear(LiteralPoolLookupCache *cache)
{
  memset(cache, 0, sizeof(*cache));
}

static inline int literal_pool_lookup_cache_find(LiteralPoolLookupCache *cache, uint32_t full_hash, Sym *sym,
                                                 int64_t imm)
{
  LiteralPoolLookupCacheEntry *entry = &cache->entries[full_hash & (LITERAL_POOL_LOOKUP_CACHE_SIZE - 1)];
  if (entry->valid && entry->hash == full_hash && entry->sym == sym && entry->imm == imm)
    return entry->pool_index;
  return -1;
}

static inline void literal_pool_lookup_cache_insert(LiteralPoolLookupCache *cache, uint32_t full_hash, Sym *sym,
                                                    int64_t imm, int pool_index)
{
  LiteralPoolLookupCacheEntry *entry = &cache->entries[full_hash & (LITERAL_POOL_LOOKUP_CACHE_SIZE - 1)];
  entry->sym = sym;
  entry->imm = imm;
  entry->pool_index = pool_index;
  entry->hash = full_hash;
  entry->valid = 1;
}

static inline int literal_pool_hash_find(TCCChainedHash *hash, ThumbLiteralPoolEntry *pool, uint32_t full_hash,
                                         Sym *sym, int64_t imm)
{
  uint32_t slot = tcc_chained_hash_bucket_head(hash, full_hash);
  while (slot)
  {
    int pool_index = (int)tcc_chained_hash_slot_to_index(slot);
    if (tcc_chained_hash_entry_hash(hash, (uint32_t)pool_index) == full_hash && pool[pool_index].sym == sym &&
        pool[pool_index].imm == imm)
      return pool_index;
    slot = tcc_chained_hash_next_slot(hash, slot);
  }
  return -1;
}

static inline void literal_pool_hash_insert(TCCChainedHash *hash, uint32_t full_hash, int pool_index)
{
  tcc_chained_hash_insert_head(hash, full_hash, (uint32_t)pool_index);
}

static void dry_run_init(void)
{
  memset(&dry_run_state, 0, sizeof(dry_run_state));
}

static void dry_run_record_push(int reg)
{
  dry_run_state.scratch_regs_pushed |= (1u << reg);
  dry_run_state.scratch_push_count++;
  if (reg == R_LR)
    dry_run_state.lr_push_count++;
}

/* Structure to save/restore thumb_gen_state for dry-run isolation */
typedef struct ThumbGenStateSnapshot
{
  int code_size;
  int literal_pool_count;
  int literal_pool_size;
  ThumbLiteralPoolEntry *literal_pool;
  Sym *cached_global_sym;
  int cached_global_reg;
  int function_argument_count;
  int call_sites_by_id_size;
  ThumbGenCallSite *call_sites_by_id;
} ThumbGenStateSnapshot;

static ThumbGenStateSnapshot dry_run_snapshot;

static void thumb_gen_state_snapshot_save(ThumbGenStateSnapshot *snap)
{
  snap->code_size = thumb_gen_state.code_size;
  snap->literal_pool_count = thumb_gen_state.literal_pool_count;
  snap->literal_pool_size = thumb_gen_state.literal_pool_size;
  snap->literal_pool = thumb_gen_state.literal_pool;
  snap->cached_global_sym = thumb_gen_state.cached_global_sym;
  snap->cached_global_reg = thumb_gen_state.cached_global_reg;
  snap->function_argument_count = thumb_gen_state.function_argument_count;
  /* call_sites_by_id is more complex - save pointer and size */
  snap->call_sites_by_id_size = thumb_gen_state.call_sites_by_id_size;
  snap->call_sites_by_id = thumb_gen_state.call_sites_by_id;
}

static void thumb_gen_state_snapshot_restore(ThumbGenStateSnapshot *snap)
{
  thumb_gen_state.code_size = snap->code_size;
  /* Free any literal pool array allocated during dry-run (if reallocated) */
  if (thumb_gen_state.literal_pool != snap->literal_pool)
  {
    tcc_free(thumb_gen_state.literal_pool);
  }
  thumb_gen_state.literal_pool = snap->literal_pool;
  thumb_gen_state.literal_pool_count = snap->literal_pool_count;
  thumb_gen_state.literal_pool_size = snap->literal_pool_size;
  thumb_gen_state.cached_global_sym = snap->cached_global_sym;
  thumb_gen_state.cached_global_reg = snap->cached_global_reg;
  thumb_gen_state.function_argument_count = snap->function_argument_count;
  /* Free any call sites created during dry-run */
  if (thumb_gen_state.call_sites_by_id != snap->call_sites_by_id)
  {
    tcc_free(thumb_gen_state.call_sites_by_id);
  }
  thumb_gen_state.call_sites_by_id = snap->call_sites_by_id;
  thumb_gen_state.call_sites_by_id_size = snap->call_sites_by_id_size;
}

/* ============================================================
 * Branch Instruction Optimization State
 * ============================================================
 * Tracks branch instructions during dry-run to select optimal
 * 16-bit vs 32-bit encodings based on actual jump distances.
 */

typedef enum
{
  BRANCH_ENC_UNKNOWN = 0,
  BRANCH_ENC_16BIT = 16,
  BRANCH_ENC_32BIT = 32
} BranchEncoding;

typedef struct BranchInfo
{
  int ir_index;            /* IR instruction index of the branch */
  int source_addr;         /* Code address where branch is emitted */
  int target_ir;           /* Target IR instruction index */
  int target_addr;         /* Target code address (computed after dry-run) */
  int offset;              /* Computed offset = target - source - 4 */
  int is_conditional;      /* 1 = conditional (JUMPIF), 0 = unconditional (JUMP) */
  BranchEncoding encoding; /* Selected encoding after analysis */
} BranchInfo;

typedef struct BranchOptState
{
  BranchInfo *branches;     /* Array of branch info */
  int branch_count;         /* Number of branches */
  int branch_capacity;      /* Allocated capacity */
  int optimization_enabled; /* Flag to enable/disable */
  int code_size_reduction;  /* Total bytes saved */
} BranchOptState;

static BranchOptState branch_opt_state;

/* Forward declarations */
static void branch_opt_init(void);
static void branch_opt_record(int ir_index, int source_addr, int target_ir, int is_conditional);
static void branch_opt_analyze(uint32_t *ir_to_code_mapping, int mapping_size);
/* Public accessor for branch encoding - returns 16 or 32 */
ST_FUNC int tcc_gen_machine_branch_opt_get_encoding(int ir_index)
{
  for (int i = 0; i < branch_opt_state.branch_count; i++)
  {
    if (branch_opt_state.branches[i].ir_index == ir_index)
    {
      return branch_opt_state.branches[i].encoding == BRANCH_ENC_16BIT ? 16 : 32;
    }
  }
  return 32; /* Conservative fallback */
}

static BranchEncoding branch_opt_get_encoding(int ir_index);

/* Check if offset fits in 16-bit conditional branch (T1 encoding)
 * Range: -256 to +254 bytes (imm8 * 2), must be even */
static int branch_fits_t1(int offset)
{
  return (offset >= -256 && offset <= 254 && (offset & 1) == 0);
}

/* Check if offset fits in 16-bit unconditional branch (T2 encoding)
 * Range: -2048 to +2046 bytes (imm11 * 2), must be even */
static int branch_fits_t2(int offset)
{
  return (offset >= -2048 && offset <= 2046 && (offset & 1) == 0);
}

/* Initialize branch optimization state */
static void branch_opt_init(void)
{
  branch_opt_state.branch_count = 0;
  branch_opt_state.optimization_enabled =
      0; /* Dry-run analysis disabled: use real-time backward branch narrowing instead */
  branch_opt_state.code_size_reduction = 0;
  if (!branch_opt_state.branches)
  {
    branch_opt_state.branch_capacity = 64;
    branch_opt_state.branches = tcc_malloc(branch_opt_state.branch_capacity * sizeof(BranchInfo));
  }
}

/* Record a branch for later optimization analysis (used by dry-run analysis path) */
static void __attribute__((unused)) branch_opt_record(int ir_index, int source_addr, int target_ir, int is_conditional)
{
  if (!branch_opt_state.optimization_enabled)
    return;

  /* Grow array if needed */
  if (branch_opt_state.branch_count >= branch_opt_state.branch_capacity)
  {
    branch_opt_state.branch_capacity *= 2;
    branch_opt_state.branches =
        tcc_realloc(branch_opt_state.branches, branch_opt_state.branch_capacity * sizeof(BranchInfo));
  }

  BranchInfo *b = &branch_opt_state.branches[branch_opt_state.branch_count++];
  b->ir_index = ir_index;
  b->source_addr = source_addr;
  b->target_ir = target_ir;
  b->target_addr = -1; /* Unknown until targets resolved */
  b->offset = 0;
  b->is_conditional = is_conditional;
  b->encoding = BRANCH_ENC_32BIT; /* Conservative default */
}

/* Analyze branch offsets and select optimal encodings.
 * Uses iterative relaxation: shrinking branches may enable more 16-bit branches.
 */
static void branch_opt_analyze(uint32_t *ir_to_code_mapping, int mapping_size)
{
  if (!branch_opt_state.optimization_enabled || branch_opt_state.branch_count == 0)
    return;

  /* Phase 1: Resolve target addresses from dry-run mapping */
  for (int i = 0; i < branch_opt_state.branch_count; i++)
  {
    BranchInfo *b = &branch_opt_state.branches[i];
    if (b->target_ir >= 0 && b->target_ir < mapping_size)
    {
      b->target_addr = ir_to_code_mapping[b->target_ir];
    }
    else
    {
      b->target_addr = b->source_addr; /* Self-loop fallback */
    }
  }

  /* Phase 2: Iterative relaxation
   * Keep trying to convert 32-bit to 16-bit until no more changes.
   * Each conversion shrinks code by 2 bytes, potentially enabling more.
   *
   * Each 16-bit branch saves 2 bytes at its source location, shifting all
   * subsequent addresses back.  For any branch i the real addresses are:
   *
   *   real(addr) = addr - 2 * #{16-bit branches whose source < addr}
   *
   * Branches are recorded in source-address order, so the source adjustment
   * for branch i is a running total (cumulative_shrink) of all 16-bit
   * branches 0..i-1.  The target may be forward (after later 16-bit
   * branches) or backward, so we scan the full branch array.
   *
   * Monotonic convergence is guaranteed: shrinking a branch can only reduce
   * (or keep equal) the magnitude of other branches' offsets, so no branch
   * ever needs to be re-widened.
   */
  int changed;
  int iterations = 0;
  const int MAX_ITERATIONS = 10; /* Prevent infinite loops */

  do
  {
    changed = 0;
    int cumulative_shrink = 0;

    for (int i = 0; i < branch_opt_state.branch_count; i++)
    {
      BranchInfo *b = &branch_opt_state.branches[i];

      /* Source adjustment: running total of 16-bit branches before this source.
       * cumulative_shrink already accounts for branches 0..i-1 that are 16-bit
       * (whether converted in this iteration or a previous one). */
      int adjusted_source = b->source_addr - cumulative_shrink;

      /* Target adjustment: count ALL 16-bit branches (any index) whose
       * original source_addr falls before this branch's target_addr. */
      int target_shrink = 0;
      for (int j = 0; j < branch_opt_state.branch_count; j++)
      {
        if (branch_opt_state.branches[j].encoding == BRANCH_ENC_16BIT &&
            branch_opt_state.branches[j].source_addr < b->target_addr)
        {
          target_shrink += 2;
        }
      }
      int adjusted_target = b->target_addr - target_shrink;

      /* Compute offset: target - (source + instruction_size)
       * For Thumb: offset = target - source - 4 (pipeline offset) */
      int offset = adjusted_target - adjusted_source - 4;
      b->offset = offset;

      /* Try to use 16-bit encoding */
      if (b->encoding == BRANCH_ENC_32BIT)
      {
        int can_use_16bit = b->is_conditional ? branch_fits_t1(offset) : branch_fits_t2(offset);

        if (can_use_16bit)
        {
          b->encoding = BRANCH_ENC_16BIT;
          changed = 1;
        }
      }

      /* Track cumulative shrink for ALL 16-bit branches (including those
       * converted in previous iterations) so that subsequent source
       * adjustments are correct. */
      if (b->encoding == BRANCH_ENC_16BIT)
      {
        cumulative_shrink += 2;
      }
    }

    iterations++;
  } while (changed && iterations < MAX_ITERATIONS);

  /* Calculate total savings */
  branch_opt_state.code_size_reduction = 0;
  for (int i = 0; i < branch_opt_state.branch_count; i++)
  {
    if (branch_opt_state.branches[i].encoding == BRANCH_ENC_16BIT)
    {
      branch_opt_state.code_size_reduction += 2;
    }
  }

  LOG_BRANCH_OPT("%d branches, %d converted to 16-bit, %d bytes saved, %d iterations", branch_opt_state.branch_count,
                 branch_opt_state.code_size_reduction / 2, branch_opt_state.code_size_reduction, iterations);
}

/* Lookup encoding decision for a given IR index */
/* Local version that returns the enum type (used by dry-run analysis path) */
static BranchEncoding __attribute__((unused)) branch_opt_get_encoding(int ir_index)
{
  for (int i = 0; i < branch_opt_state.branch_count; i++)
  {
    if (branch_opt_state.branches[i].ir_index == ir_index)
    {
      return branch_opt_state.branches[i].encoding;
    }
  }
  return BRANCH_ENC_32BIT; /* Conservative fallback */
}

/* Public interface for branch optimization */
ST_FUNC void tcc_gen_machine_branch_opt_analyze(uint32_t *ir_to_code_mapping, int mapping_size)
{
  branch_opt_analyze(ir_to_code_mapping, mapping_size);
}

ST_FUNC void tcc_gen_machine_branch_opt_init(void)
{
  branch_opt_init();
}

/* Reset the MOV-coalescing and STR->LDR redundant-reload caches.  Called
 * at IR instruction boundaries because any IR op can be the target of a
 * branch from elsewhere: arriving via jump, the runtime register and
 * memory state is not what the emission-order state would predict, so
 * cross-IR matching is unsafe.  Within a single IR op the backend emits
 * straight-line code and both peepholes are sound. */
ST_FUNC void tcc_gen_machine_mov_coalesce_reset(void)
{
  mov_equiv_reset_all();
  tcc_gen_machine_strldr_cache_reset();
}

/* Reset only the MOV-coalescing register-equivalence cache.  Unlike the
 * STR->LDR memory cache, the GPR value-equivalence cache stays sound across
 * straight-line IR-op boundaries: every instruction the backend emits passes
 * through the ot() updater, which invalidates the destination register (and
 * a `bl`/unknown opcode triggers a full reset, covering call clobbers).  So
 * the only place a reset is genuinely required is a real control-flow merge:
 * arriving at a branch target, the emission-order equivalences from the
 * fall-through predecessor do not describe the register state on the
 * jumped-from path.  codegen.c therefore calls this only at jump targets,
 * letting cross-IR `mov` chains (e.g. a soft-float call result copied to its
 * home pair and then to the next call's argument pair) coalesce away. */
ST_FUNC void tcc_gen_machine_mov_equiv_reset(void)
{
  mov_equiv_reset_all();
}

/* Public interface for dry-run code generation */
ST_FUNC void tcc_gen_machine_dry_run_init(void)
{
  dry_run_init();
}

ST_FUNC void tcc_gen_machine_dry_run_start(void)
{
  dry_run_state.active = 1;
  /* Allocate dry-run literal pool if not already allocated */
  if (!dry_run_literal_pool)
  {
    dry_run_literal_pool_size = 64;
    dry_run_literal_pool = tcc_malloc(dry_run_literal_pool_size * sizeof(ThumbLiteralPoolEntry));
  }
  dry_run_literal_pool_count = 0;
  /* Clear the shared hash table for dry-run pass */
  literal_pool_hash_clear(&literal_pool_hash);
  literal_pool_lookup_cache_clear(&literal_pool_last_lookup);
  /* Save thumb_gen_state before dry-run */
  thumb_gen_state_snapshot_save(&dry_run_snapshot);
  /* Reset state that should start fresh for dry-run */
  thumb_gen_state.code_size = 0;
  thumb_gen_state.literal_pool_count = 0;
  thumb_gen_state.cached_global_sym = NULL;
  thumb_gen_state.cached_global_reg = PREG_NONE;
  thumb_gen_state.function_argument_count = 0;
  /* call_sites_by_id - don't modify, just track that we saved it */
  imm_cache_reset_all();
}

ST_FUNC void tcc_gen_machine_dry_run_end(void)
{
  dry_run_state.active = 0;
  /* Restore thumb_gen_state after dry-run */
  thumb_gen_state_snapshot_restore(&dry_run_snapshot);
  imm_cache_reset_all();
  /* Clear the literal pool hash table so that stale dry-run indices
   * don't cause real-pass entries to be misidentified as shared. */
  literal_pool_hash_clear(&literal_pool_hash);
  literal_pool_lookup_cache_clear(&literal_pool_last_lookup);
  /* Note: we keep dry_run_literal_pool allocated for reuse */
}

ST_FUNC int tcc_gen_machine_dry_run_get_lr_push_count(void)
{
  return dry_run_state.lr_push_count;
}

ST_FUNC uint32_t tcc_gen_machine_dry_run_get_scratch_regs_pushed(void)
{
  return dry_run_state.scratch_regs_pushed;
}

/* Check if dry-run mode is currently active */
ST_FUNC int tcc_gen_machine_dry_run_is_active(void)
{
  return dry_run_state.active;
}

/* Reset scratch register state between dry-run and real passes */
ST_FUNC void tcc_gen_machine_reset_scratch_state(void)
{
  /* When text_and_data_separation is active, R9 holds the GOT base and must
   * NEVER be used as a scratch register. Permanently exclude it. */
  scratch_global_exclude = text_and_data_separation ? (1u << R9) : 0;
  scratch_push_count = 0;
  scratch_save_slot = 0;
  memset(scratch_push_stack, 0, sizeof(scratch_push_stack));
  memset(scratch_push_type, 0, sizeof(scratch_push_type));
  real_run_scratch_push_detected = 0;
}

/* Returns 1 if any scratch PUSH was emitted during the real run. */
ST_FUNC int tcc_gen_machine_real_run_had_scratch_push(void)
{
  return real_run_scratch_push_detected;
}

/* Per-instruction scratch tracking (Phase 3 constraint collection).
 * Call reset before each mop dispatched instruction; call count after to
 * retrieve the number of scratch registers allocated for that instruction.
 * Works in both dry-run and real-emit passes so the two can be compared. */
ST_FUNC void tcc_gen_machine_insn_scratch_reset(void)
{
  g_insn_scratch_allocs = 0;
  g_insn_scratch_saves = 0;
}

ST_FUNC int tcc_gen_machine_insn_scratch_count(void)
{
  return g_insn_scratch_allocs;
}

/* Returns a bitmask of registers that required PUSH during the most recent
 * instruction (i.e., no free scratch register was available and one had to
 * be saved to the stack).  Works in both dry-run and real-emit modes. */
ST_FUNC uint16_t tcc_gen_machine_insn_scratch_saves_mask(void)
{
  return g_insn_scratch_saves;
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

  LOG_SCRATCH("get_scratch_reg: input_exclude=0x%x global_exclude=0x%x", exclude_regs, scratch_global_exclude);

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
      LOG_SCRATCH("-> returning reg=%d (free) exclude=0x%x", reg, exclude_regs);
      result.reg = reg;
      result.saved = 0;
      /* Update global exclude so subsequent calls won't return the same register.
       * This prevents nested scratch allocations from silently reusing and
       * clobbering a still-live operand (e.g. during constant materialization). */
      scratch_global_exclude |= (1u << reg);
      return result;
    }

    /* Fallback path: a callee-saved register R4-R11 already pushed by the
     * prolog AND not live at this instruction can be used as scratch for
     * free.  The prolog/epilog save/restore makes the clobber invisible to
     * the caller.  Gated by !dry_run_active so dry-run never sees a value
     * different from real-run: pushed_registers is only valid after the
     * prolog has actually run, which is real-run.  R7 (FP) is reserved.
     * R9 is reserved as GOT base when text_and_data_separation is on. */
    if (!dry_run_state.active && pushed_registers && reg == PREG_NONE)
    {
      uint32_t reserved = (1u << R_FP);
      if (tcc_state->text_and_data_separation)
        reserved |= (1u << 9);
      uint32_t live = 0;
      if (ir->ls.live_regs_by_instruction && ir->codegen_instruction_idx >= 0 &&
          ir->codegen_instruction_idx < ir->ls.live_regs_by_instruction_size)
        live = ir->ls.live_regs_by_instruction[ir->codegen_instruction_idx];
      uint32_t candidate = pushed_registers & 0x0FF0u & ~exclude_regs & ~live & ~reserved;
      if (candidate)
      {
        int sreg = (int)__builtin_ctz(candidate);
        LOG_SCRATCH("-> returning reg=%d (pre-pushed callee-saved, dead here) exclude=0x%x", sreg, exclude_regs);
        result.reg = sreg;
        result.saved = 0;
        scratch_global_exclude |= (1u << sreg);
        return result;
      }
    }
  }

  int reg_to_save = -1;
  int lr_saved_in_prologue = 0;
no_free_reg:
  /* lr_saved_in_prologue needs to be computed here to satisfy compiler flow analysis */
  lr_saved_in_prologue = (pushed_registers & (1u << R_LR)) ? 1 : 0;

  /* In non-leaf functions OR when LR was pushed in prologue (e.g., due to dry-run
   * discovering it would be needed as scratch), LR is already saved.
   * We can use it as scratch without push/pop since the epilog will restore it.
   * This is more efficient than pushing another register.
   */
  if (ir && (lr_saved_in_prologue || !ir->leaffunc) && !(exclude_regs & (1 << R_LR)))
  {
    /* LR is saved at prologue, use it freely */
    result.reg = R_LR;
    result.saved = 0; /* No push needed - already saved at prologue */
    scratch_global_exclude |= (1u << R_LR);
    return result;
  }

  /* No free register found - we need to save one to the stack.
   * Prefer R0-R3: PUSH/POP and most ALU ops use 16-bit Thumb encoding,
   * whereas R_IP (R12) forces 32-bit encoding for every instruction. */
  for (int r = 0; r <= 3; ++r)
  {
    if (!(exclude_regs & (1 << r)))
    {
      reg_to_save = r;
      break;
    }
  }

  if (reg_to_save < 0 && ir && ir->leaffunc && !(exclude_regs & (1 << R_LR)))
  {
    reg_to_save = R_LR;
  }

  if (reg_to_save < 0 && !(exclude_regs & (1 << R_IP)))
  {
    reg_to_save = R_IP;
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
  LOG_SCRATCH("WARNING: no free scratch register! Saving r%d to stack", reg_to_save);

  /* Dry run: record what we would push, but don't emit */
  if (dry_run_state.active)
  {
    dry_run_record_push(reg_to_save);
    /* Return as if it's free for consistent allocation decisions */
    result.reg = reg_to_save;
    result.saved = 0;
    result.would_save = 1; /* Phase 3: flag that a push would be needed */
    scratch_global_exclude |= (1u << reg_to_save);
    return result;
  }

  /* When FP is omitted, use STR to the pre-reserved scratch save area instead
   * of PUSH, to avoid moving SP (which would break SP-relative addressing). */
  if (!tcc_state->need_frame_pointer && ir && ir->scratch_save_size > 0 &&
      scratch_save_slot < (ir->scratch_save_size / 4))
  {
    int frame_offset = ir->scratch_save_base + (scratch_save_slot * 4);
    int sp_offset = allocated_stack_size + frame_offset;
    if (!store_word_to_base(reg_to_save, R_SP, sp_offset, 0))
      tcc_error("compiler_error: scratch save STR failed (offset %d)", sp_offset);
    result.reg = reg_to_save;
    result.saved = 2; /* 2 = saved to scratch area (not PUSH) */
    result.would_save = 1;
    if (scratch_push_count < 128)
    {
      scratch_push_type[scratch_push_count] = 2;
      scratch_push_stack[scratch_push_count++] = reg_to_save;
    }
    scratch_save_slot++;
    return result;
  }

  ot_check(th_push(1 << reg_to_save));
  real_run_scratch_push_detected = 1;
  result.reg = reg_to_save;
  result.saved = 1;
  result.would_save = 1; /* Phase 3: push was needed */
  /* Track push ORDER - we must POP in reverse order since ARM POP with register
   * lists pops in register-number order, not stack order. */
  if (scratch_push_count < 128)
  {
    scratch_push_type[scratch_push_count] = 1;
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
    imm_cache_invalidate_reg(alloc->reg);
  /* Dry run: don't emit pop, just update tracking */
  if (dry_run_state.active)
  {
    if (alloc->saved)
    {
      /* Track that we would have popped */
      if (scratch_push_count > 0 && scratch_push_stack[scratch_push_count - 1] == alloc->reg)
      {
        scratch_push_count--;
      }
      alloc->saved = 0;
    }
    /* Release from global exclude */
    if (alloc->reg >= 0 && alloc->reg < 32)
    {
      scratch_global_exclude &= ~(1u << alloc->reg);
    }
    return;
  }

  if (alloc->saved == 2)
  {
    /* Saved to scratch area (FP omitted path): restore via LDR */
    TCCIRState *ir = tcc_state->ir;
    if (scratch_save_slot > 0)
      scratch_save_slot--;
    int frame_offset = ir->scratch_save_base + (scratch_save_slot * 4);
    int sp_offset = allocated_stack_size + frame_offset;
    if (!load_word_from_base(alloc->reg, R_SP, sp_offset, 0))
      tcc_error("compiler_error: scratch restore LDR failed (offset %d)", sp_offset);
    alloc->saved = 0;
    if (scratch_push_count > 0 && scratch_push_stack[scratch_push_count - 1] == alloc->reg)
      scratch_push_count--;
    scratch_global_exclude &= ~(1u << alloc->reg);
  }
  else if (alloc->saved == 1)
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
        LOG_SCRATCH("WARNING: restore_scratch_reg out of order; deferring POP "
                    "reg=%d (top=%d)",
                    alloc->reg, scratch_push_stack[scratch_push_count - 1]);
      }
      else
      {
        LOG_SCRATCH("WARNING: restore_scratch_reg with empty push stack; deferring POP reg=%d", alloc->reg);
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
  /* Dry run: don't emit pops, just reset tracking */
  if (dry_run_state.active)
  {
    scratch_push_count = 0;
    scratch_save_slot = 0;
    scratch_global_exclude = text_and_data_separation ? (1u << R9) : 0;
    return;
  }

  /* Restore in reverse order */
  for (int i = scratch_push_count - 1; i >= 0; i--)
  {
    int reg = scratch_push_stack[i];
    LOG_SCRATCH("auto-restoring r%d (push order %d, type %d)", reg, i, scratch_push_type[i]);
    if (scratch_push_type[i] == 2)
    {
      /* Saved to scratch area: restore via LDR */
      TCCIRState *ir = tcc_state->ir;
      if (scratch_save_slot > 0)
        scratch_save_slot--;
      int frame_offset = ir->scratch_save_base + (scratch_save_slot * 4);
      int sp_offset = allocated_stack_size + frame_offset;
      if (!load_word_from_base(reg, R_SP, sp_offset, 0))
        tcc_error("compiler_error: scratch auto-restore LDR failed (offset %d)", sp_offset);
    }
    else
    {
      /* Saved via PUSH: restore via POP */
      ot_check(th_pop(1 << reg));
    }
  }
  scratch_push_count = 0;
  /* Also reset global exclude for next IR instruction.
   * Keep R9 excluded if text_and_data_separation is active. */
  scratch_global_exclude = text_and_data_separation ? (1u << R9) : 0;
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
    LOG_SCRATCH("ot_check FAIL: opcode=0x%x ind=0x%x ir_op=%d", op.opcode, (unsigned)ind, g_debug_current_op);
    tcc_error("compiler_error: received invalid opcode: 0x%x\n", op.opcode);
  }
  return ot(op);
}

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
  cache->last_emit_kind = 0;
  cache->last_emit_ind = 0;
  cache->last_emit_reg = 0;
  cache->last_emit_offset = 0;
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

/* ---------------------------------------------------------------------------
 * Redundant MOV reg coalescing
 *
 * Tracks which physical registers currently hold the same value and drops
 * `MOV Rd, Rm` when Rd is already known to equal Rm.  Equivalence classes
 * are represented by each register's class-representative in mov_equiv[];
 * two registers are equal iff their representatives match.
 *
 * Updates:
 *   MOV Rd, Rm             -> mov_equiv[Rd] := mov_equiv[Rm]   (Rd joins Rm)
 *   any other write to Rc  -> mov_equiv[Rc] := Rc              (Rc new class)
 *   unclassified opcode    -> full reset                       (conservative)
 *
 * Only applies to plain `MOV Rd, Rm` with no shift and no flag-setting (the
 * T1 16-bit encoding and the no-shift/no-flags T2 32-bit encoding).  Any
 * shifted / flag-setting MOV reads flags or transforms Rm and is left alone.
 * --------------------------------------------------------------------------- */

static uint8_t mov_equiv[16];

/* Count of conditional instructions still pending inside an IT/ITx/ITxy/ITxyz
 * block.  While this is non-zero the opcode stream seen by ot() is
 * conditionally executed; cache updates must treat destinations as "may or
 * may not be written", not as guaranteed assignments. */
static int mov_equiv_it_pending;

/* Immediate-value cache: tracks the last pure-integer constant loaded into
 * each register by tcc_machine_load_constant (no symbol involved).  Persists
 * across IR instruction boundaries so consecutive STORE instructions that
 * materialise the same constant can skip the redundant MOV.  Reset at jump
 * targets and function calls. */
/* Per-register materialisation cache.  `sym == NULL` means the register holds
 * the plain constant `value`; `sym != NULL` means it holds the address of that
 * symbol plus addend `value` (so a later reference to the same global address
 * can skip the redundant literal-pool load).  Invalidated per-register on every
 * clobbering emit and at IR boundaries, just like the constant cache. */
static struct { int64_t value; Sym *sym; uint8_t valid; } imm_cache[16];

static void imm_cache_reset_all(void)
{
  for (int i = 0; i < 16; i++)
  {
    imm_cache[i].valid = 0;
    imm_cache[i].sym = NULL;
  }
}

static void imm_cache_invalidate_reg(int reg)
{
  if (reg >= 0 && reg < 16)
    imm_cache[reg].valid = 0;
}

static void mov_equiv_reset_all(void)
{
  for (int i = 0; i < 16; i++)
    mov_equiv[i] = (uint8_t)i;
  mov_equiv_it_pending = 0;
}

/* Decode the IT instruction (Thumb-2 16-bit, opcode 0xBF<cond><mask>) and
 * return the number of instructions that will execute conditionally after
 * it — 1..4 depending on which bit of the mask is lowest-set.  Returns 0
 * when the opcode is not an IT (mask == 0 is a plain NOP/hint). */
static int mov_equiv_it_block_length(thumb_opcode op)
{
  if (op.size != 2)
    return 0;
  uint16_t hw = (uint16_t)(op.opcode & 0xFFFF);
  if ((hw & 0xFF00) != 0xBF00)
    return 0;
  uint16_t mask = hw & 0x0F;
  if (mask == 0)
    return 0; /* NOP-hint encodings (NOP, YIELD, WFE, ...) */
  if (mask & 0x1)
    return 4;
  if (mask & 0x2)
    return 3;
  if (mask & 0x4)
    return 2;
  return 1; /* mask & 0x8 */
}

static void mov_equiv_invalidate_reg(int reg)
{
  if (reg < 0 || reg >= 16)
    return;
  /* Any other register whose representative was `reg` becomes independent
   * of reg's new (unknown) value.  Give each such register its own class. */
  uint8_t old_rep = mov_equiv[reg];
  for (int i = 0; i < 16; i++)
  {
    if (i != reg && mov_equiv[i] == old_rep)
      mov_equiv[i] = (uint8_t)i;
  }
  mov_equiv[reg] = (uint8_t)reg;
}

static void mov_equiv_record_mov(int rd, int rm)
{
  if (rd < 0 || rd >= 16 || rm < 0 || rm >= 16)
  {
    mov_equiv_reset_all();
    return;
  }
  /* First invalidate Rd's old equivalences (Rd stops being equal to whatever
   * it was before), then merge into Rm's class. */
  mov_equiv_invalidate_reg(rd);
  mov_equiv[rd] = mov_equiv[rm];
}

/* Emit `MOV Rd, Rm` unless the register-equivalence cache already says
 * Rd currently holds the same value as Rm, in which case the MOV is a
 * no-op and nothing is emitted.  Only the no-shift / no-flag-set forms
 * participate in coalescing (identical to decode_mov_reg_plain); any
 * caller passing a shift, setting flags, or using IT-conditional forms
 * always emits through ot_check so that the semantics of those MOVs is
 * preserved. */
static int ot_check_mov_reg(uint32_t rd, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                            thumb_enforce_encoding enc, bool in_it)
{
  const int coalesceable = (flags != FLAGS_BEHAVIOUR_SET) && !in_it && (shift.type == THUMB_SHIFT_NONE) && (rd < 16) &&
                           (rm < 16) && thumb_gen_state.generating_function;
  if (coalesceable && (rd == rm || mov_equiv[rd] == mov_equiv[rm]))
  {
    /* Elided at the call site: ot() is never reached, so ind and code_size
     * only reflect instructions that really got emitted.  The cache is
     * already consistent (rd is equal to rm), so no update is needed. */
    return 0;
  }
  thumb_opcode mov_op = th_mov_reg(rd, rm, flags, shift, enc, in_it);
  return ot_check(mov_op);
}

/* Return 1 if `op` is a plain MOV Rd, Rm with no shift and no flag set,
 * filling *rd_out / *rm_out.  Accepts the 16-bit T1 high-register form and
 * the 32-bit T2 form when shift/flags are zero. */
static int decode_mov_reg_plain(thumb_opcode op, int *rd_out, int *rm_out)
{
  if (op.size == 2)
  {
    uint16_t hw = (uint16_t)(op.opcode & 0xFFFF);
    /* T1 MOV high-register: 0100 0110 D Rm4 Rd3 */
    if ((hw & 0xFF00) == 0x4600)
    {
      int rd = ((hw >> 4) & 0x08) | (hw & 0x07);
      int rm = (hw >> 3) & 0x0F;
      *rd_out = rd;
      *rm_out = rm;
      return 1;
    }
    return 0;
  }
  if (op.size == 4)
  {
    uint16_t hi = (uint16_t)((op.opcode >> 16) & 0xFFFF);
    uint16_t lo = (uint16_t)(op.opcode & 0xFFFF);
    /* T2 MOV register, no shift, no flag set: EA4F 0<rd>0<rm>.
     * Opcode layout (ARM ARM): 11101010 0100 1111 | 0 imm3 Rd imm2 type Rm
     * For plain MOV (no shift): imm3 = 0, imm2 = 0, type = 00 (LSL).
     * S bit distinguishes MOV/MOVS: hi[20] = 0 for MOV, 1 for MOVS. */
    if (hi == 0xEA4F && (lo & 0x70F0) == 0)
    {
      int rd = (lo >> 8) & 0x0F;
      int rm = lo & 0x0F;
      *rd_out = rd;
      *rm_out = rm;
      return 1;
    }
    return 0;
  }
  return 0;
}

/* ---------------------------------------------------------------------------
 * STR -> LDR redundant-reload peephole
 *
 * Tracks recent immediate-offset STR Rt, [Rn, #imm] emissions and skips the
 * subsequent LDR Rt, [Rn, #imm] at the call site when Rt is still known to
 * hold the stored value.  The cache is reset at every IR instruction
 * boundary (via tcc_gen_machine_mov_coalesce_reset, same hook as plan C)
 * so that cross-IR equivalences cannot be exploited — any IR op may be a
 * branch target, and the runtime register/memory state on an entry-by-jump
 * path is not what the emission-order state predicts.
 *
 * Only puw == 6 (P=1, U=1, W=0 — no writeback) STR/LDR forms are tracked.
 * The classifier below recognises the T1 16-bit, T2 16-bit SP-relative, and
 * T3 32-bit encodings (those that cover the common stack-spill path).
 * --------------------------------------------------------------------------- */

typedef struct StrLdrCacheEntry
{
  uint8_t valid;
  uint8_t rt;
  uint8_t rn;
  uint8_t size; /* 2 or 4 */
  int imm;
  uint32_t puw;
} StrLdrCacheEntry;

#define STRLDR_CACHE_CAPACITY 8
static StrLdrCacheEntry strldr_cache[STRLDR_CACHE_CAPACITY];
static int strldr_cache_count;

ST_FUNC void tcc_gen_machine_strldr_cache_reset(void)
{
  strldr_cache_count = 0;
}

ST_FUNC void tcc_gen_machine_imm_cache_reset(void)
{
  imm_cache_reset_all();
}

ST_FUNC void tcc_gen_machine_imm_cache_invalidate_live(uint32_t live_mask)
{
  for (int i = 0; i < 16; i++) {
    if (live_mask & (1u << i))
      imm_cache[i].valid = 0;
  }
}

/* Invalidate entries where the given register is either the stored value
 * (Rt) or the base register (Rn).  Called when a subsequent instruction
 * writes to that register. */
static void strldr_cache_invalidate_reg(int reg)
{
  for (int i = 0; i < strldr_cache_count; i++)
  {
    StrLdrCacheEntry *e = &strldr_cache[i];
    if (e->valid && (e->rt == reg || e->rn == reg))
      e->valid = 0;
  }
}

static void strldr_cache_record_str(int rt, int rn, int imm, uint32_t puw, int size)
{
  if (puw != 6)
  {
    tcc_gen_machine_strldr_cache_reset();
    return;
  }
  /* Overwriting the same slot invalidates any prior cache entry for it. */
  for (int i = 0; i < strldr_cache_count; i++)
  {
    StrLdrCacheEntry *e = &strldr_cache[i];
    if (e->valid && e->rn == rn && e->imm == imm)
      e->valid = 0;
  }
  if (strldr_cache_count >= STRLDR_CACHE_CAPACITY)
  {
    tcc_gen_machine_strldr_cache_reset();
  }
  StrLdrCacheEntry *e = &strldr_cache[strldr_cache_count++];
  e->valid = 1;
  e->rt = (uint8_t)rt;
  e->rn = (uint8_t)rn;
  e->imm = imm;
  e->puw = puw;
  e->size = (uint8_t)size;
}

/* Return 1 when a matching unclobbered STR entry exists that makes this
 * LDR redundant.  Matches on all fields so a 16-bit LDR won't be elided
 * against a 32-bit STR (and vice versa) — the encodings might pick
 * different scale semantics. */
static int strldr_cache_try_match_ldr(int rt, int rn, int imm, uint32_t puw, int size)
{
  if (puw != 6)
    return 0;
  for (int i = 0; i < strldr_cache_count; i++)
  {
    StrLdrCacheEntry *e = &strldr_cache[i];
    if (!e->valid)
      continue;
    if (e->rt == rt && e->rn == rn && e->imm == imm && e->puw == puw && e->size == size)
      return 1;
  }
  return 0;
}

/* Decode T1/T2/T3 STR/LDR immediate-offset forms with no writeback.
 * Returns 1 and fills outputs when the opcode matches, 0 otherwise.
 * *is_str_out is 1 for STR, 0 for LDR. */
static int decode_str_ldr_imm(thumb_opcode op, int *is_str_out, int *rt_out, int *rn_out, int *imm_out,
                              uint32_t *puw_out)
{
  if (op.size == 2)
  {
    uint16_t hw = (uint16_t)(op.opcode & 0xFFFF);
    /* T1: 0b01100 = STR, 0b01101 = LDR (imm5 word-scaled, rn<8, rt<8). */
    if ((hw & 0xF000) == 0x6000)
    {
      int is_ldr = (hw >> 11) & 1;
      *is_str_out = !is_ldr;
      *rt_out = hw & 0x7;
      *rn_out = (hw >> 3) & 0x7;
      *imm_out = ((hw >> 6) & 0x1F) << 2;
      *puw_out = 6;
      return 1;
    }
    /* T2: SP-relative. 0b10010 = STR, 0b10011 = LDR (imm8 word-scaled). */
    if ((hw & 0xF000) == 0x9000)
    {
      int is_ldr = (hw >> 11) & 1;
      *is_str_out = !is_ldr;
      *rt_out = (hw >> 8) & 0x7;
      *rn_out = R_SP;
      *imm_out = (hw & 0xFF) << 2;
      *puw_out = 6;
      return 1;
    }
    /* STRB/LDRB imm5: 0111 0xxx (STR) / 0111 1xxx (LDR). */
    if ((hw & 0xF000) == 0x7000)
    {
      *is_str_out = !((hw >> 11) & 1);
      *rt_out = hw & 0x7;
      *rn_out = (hw >> 3) & 0x7;
      *imm_out = (hw >> 6) & 0x1F;
      *puw_out = 6;
      return 1;
    }
    /* STRH/LDRH imm5: 1000 0xxx (STR) / 1000 1xxx (LDR). */
    if ((hw & 0xF000) == 0x8000)
    {
      *is_str_out = !((hw >> 11) & 1);
      *rt_out = hw & 0x7;
      *rn_out = (hw >> 3) & 0x7;
      *imm_out = ((hw >> 6) & 0x1F) << 1;
      *puw_out = 6;
      return 1;
    }
    return 0;
  }
  if (op.size == 4)
  {
    uint16_t hi = (uint16_t)((op.opcode >> 16) & 0xFFFF);
    uint16_t lo = (uint16_t)(op.opcode & 0xFFFF);
    /* T3: STR/LDR variants with imm12 (byte/half/word): hi[22:21]=size,
     * hi[20]=L.  0xF88x=STRB.W, 0xF89x=LDRB.W, 0xF8Ax=STRH.W,
     * 0xF8Bx=LDRH.W, 0xF8Cx=STR.W, 0xF8Dx=LDR.W. */
    if ((hi & 0xFF80) == 0xF880)
    {
      int is_ldr = (hi >> 4) & 1;
      int rn = hi & 0xF;
      if (rn == 0xF)
        return 0; /* PC-relative literal load; skip. */
      *is_str_out = !is_ldr;
      *rn_out = rn;
      *rt_out = (lo >> 12) & 0xF;
      *imm_out = lo & 0xFFF;
      *puw_out = 6;
      return 1;
    }
    return 0;
  }
  return 0;
}

/* Emit LDR Rt, [Rn, #imm] unless the STR-cache already knows Rt still
 * holds [Rn+imm] from an unclobbered earlier STR, in which case emission
 * is skipped entirely.  ot() is never called in the elided path, so `ind`
 * and code_size only advance for real emissions — same contract as the
 * MOV coalescing helper. */
static int ot_check_ldr_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc)
{
  thumb_opcode ins = th_ldr_imm(rt, rn, imm, puw, enc);
  if (thumb_gen_state.generating_function && puw == 6 && ins.size != 0 &&
      strldr_cache_try_match_ldr((int)rt, (int)rn, imm, puw, ins.size))
  {
    /* Redundant reload: Rt still holds [Rn+imm] from an earlier STR that
     * has not been clobbered.  No emission, no cache update needed — the
     * existing entry remains accurate. */
    return 0;
  }
  return ot_check(ins);
}

/* Emit STR Rt, [Rn, #imm].  Always emits (STR cannot be elided); the
 * cache-record side effect happens inside ot() once the opcode is
 * classified, so there is nothing extra to do here other than go through
 * the standard ot_check path.  Kept as a dedicated helper only for
 * symmetry with ot_check_ldr_imm — callers use it so future refinements
 * (e.g. dropping a dead store that follows another store to the same
 * slot) can land in one place. */
static int ot_check_str_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc)
{
  thumb_opcode ins = th_str_imm(rt, rn, imm, puw, enc);
  return ot_check(ins);
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
  return "/lib/ld-linux.so";
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
  if (!literal_pool_hash.buckets)
    tcc_chained_hash_init(&literal_pool_hash, LITERAL_POOL_HASH_BUCKET_COUNT, thumb_gen_state.literal_pool_size);
  else
    tcc_chained_hash_reserve(&literal_pool_hash, thumb_gen_state.literal_pool_size);
  thumb_gen_state.generating_function = 0;
  thumb_gen_state.code_size = 0;
  thumb_gen_state.cached_global_sym = NULL;
  thumb_gen_state.cached_global_reg = PREG_NONE;
  /* Clear the hash table for O(1) lookups */
  literal_pool_hash_clear(&literal_pool_hash);
  literal_pool_lookup_cache_clear(&literal_pool_last_lookup);
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

static const char *arm_fpu_type_to_mfpu_str(unsigned char fpu_type)
{
  switch (fpu_type)
  {
  case ARM_FPU_FPV4_SP_D16:
    return "fpv4-sp-d16";
  case ARM_FPU_FPV5_SP_D16:
    return "fpv5-sp-d16";
  case ARM_FPU_FPV5_D16:
    return "fpv5-d16";
  case ARM_FPU_NONE:
    return "none";
  default:
    return NULL;
  }
}

const FloatingPointConfig *arm_determine_fpu_config(struct TCCState *s)
{
  if (s->fpu_type == 0 || s->fpu_type == ARM_FPU_NONE)
  {
    return &arm_soft_fpu_config;
  }

  switch (s->fpu_type)
  {
  case ARM_FPU_FPV4_SP_D16:
  case ARM_FPU_FPV5_SP_D16:
    return &arm_fpv5_sp_d16_fpu_config;
  case ARM_FPU_FPV5_D16:
    return &arm_fpv5_d16_fpu_config;
  default:
    fprintf(stderr, "unsupported FPU type: %d for ARM architecture", s->fpu_type);
    exit(1);
    return NULL;
  }
}

ST_FUNC void arm_init(struct TCCState *s)
{
  tcc_ir_ssa_opt_arm_register();
  arm_target_init(s->march_str, arm_fpu_type_to_mfpu_str(s->fpu_type), NULL, 0);

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

  s->registers_for_allocator = 13; /* r0-r12: ip is caller-saved, available for allocation */
  caller_saved_registers = (1 << ARM_R0) | (1 << ARM_R1) | (1 << ARM_R2) | (1 << ARM_R3) | (1 << ARM_R12);

  /* On yasos with no-pic-data-is-text-relative, R9 holds the GOT base and is
   * caller-saved: callees (compiled by other toolchains) may clobber it, so
   * the compiler must save/restore R9 around every function call. */
  if (s->text_and_data_separation)
    caller_saved_registers |= (1 << ARM_R9);

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

  if (!s->pic && !s->text_and_data_separation)
  {
    s->registers_map_for_allocator |= (1 << ARM_R9);
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
  tcc_free(dry_run_literal_pool);
  tcc_chained_hash_destroy(&literal_pool_hash);
  thumb_gen_state.literal_pool = NULL;
  dry_run_literal_pool = NULL;
  thumb_gen_state.literal_pool_size = 0;
  thumb_gen_state.literal_pool_count = 0;
  dry_run_literal_pool_size = 0;
  dry_run_literal_pool_count = 0;
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

  /* During dry-run, don't actually write to section data.
   * Just update ind to track code size. */
  if (dry_run_state.active)
  {
    ind += 2;
    return;
  }

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

  /* During dry-run, we still need to generate the literal pool to ensure
   * code addresses match the real pass. The o() function will handle not
   * writing to section data during dry-run, but will increment ind. */
  if (thumb_gen_state.literal_pool_count == 0)
  {
    thumb_gen_state.code_size = 0;
    return;
  }

  generating_pool = 1;
  const int this_pool = ++pool_seq;

  /* Use dry-run pool during dry-run, otherwise use the real pool */
  ThumbLiteralPoolEntry *pool = dry_run_state.active ? dry_run_literal_pool : thumb_gen_state.literal_pool;
  int pool_count = dry_run_state.active ? dry_run_literal_pool_count : thumb_gen_state.literal_pool_count;

  /* Count unique literals to calculate pool size */
  int pool_size = 0;
  for (int i = 0; i < pool_count; i++)
  {
    if (pool[i].shared_index == -1)
    {
      int entry_size = (pool[i].data_size == 8) ? 8 : 4;
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
    ot_check(th_nop(ENFORCE_ENCODING_16BIT));
  }

  /* Array to store the output position of each unique literal */
  int *literal_positions = tcc_mallocz(pool_count * sizeof(int));

  th_sym_d();

  /* First pass: emit unique literals and record their positions */
  for (int i = 0; i < pool_count; i++)
  {
    ThumbLiteralPoolEntry *entry = &pool[i];
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
      /* Skip relocation creation during dry-run - relocations should only be
       * created during the real code generation pass. */
      if (!dry_run_state.active && entry->relocation != -1 && entry->sym)
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
  for (int i = 0; i < pool_count; i++)
  {
    ThumbLiteralPoolEntry *entry = &pool[i];
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
  /* Clear the hash table after flushing pool */
  literal_pool_hash_clear(&literal_pool_hash);
  literal_pool_lookup_cache_clear(&literal_pool_last_lookup);
}

static void th_literal_pool_reserve_upcoming_bytes(int upcoming_bytes)
{
  if (!thumb_gen_state.generating_function)
    return;

  int pool_count = dry_run_state.active ? dry_run_literal_pool_count : thumb_gen_state.literal_pool_count;
  if (pool_count == 0)
    return;

  if (thumb_gen_state.code_size + pool_count * 4 + upcoming_bytes >= 1020)
    th_literal_pool_generate();
}

int is_valid_opcode(thumb_opcode op)
{
  return (op.size == 2 || op.size == 4);
}

/* Check whether a Thumb/Thumb-2 instruction writes to R9.
 * Returns the destination register number if it can be decoded, or -1.
 * Only checks data-processing / move / load instructions, NOT push/pop/stm/ldm
 * (those legitimately reference R9 for save/restore around calls). */
/* Detect instructions that set flags only and write no GPR — CMP, CMN, TST,
 * TEQ in all their common Thumb-1 / Thumb-2 encodings.  thumb_decode_dest_reg
 * returns -1 for these, which would otherwise trigger the conservative
 * full-cache reset in ot().  Recognising them keeps the mov-equiv and
 * imm-in-reg caches alive across a CMP, which lets a follow-up redundant
 * load_immediate elide. */
static int thumb_op_is_pure_flag_setter(thumb_opcode op)
{
  uint32_t w = op.opcode;
  if (op.size == 2)
  {
    uint16_t hw = (uint16_t)(w & 0xFFFF);
    /* T1 16-bit CMP imm8 (low regs):       00101 Rd3 iiii iiii  (0x28-0x2F) */
    if ((hw & 0xF800) == 0x2800)
      return 1;
    /* T1 16-bit CMP reg (low regs):        0100 0010 10Rm3 Rn3  (0x4280) */
    if ((hw & 0xFFC0) == 0x4280)
      return 1;
    /* T1 16-bit TST reg (low regs):        0100 0010 00Rm3 Rn3  (0x4200) */
    if ((hw & 0xFFC0) == 0x4200)
      return 1;
    /* T2 16-bit CMP/CMN reg (high regs):   0100 0101 D Rm4 Rn3  (0x4500) */
    if ((hw & 0xFF00) == 0x4500)
      return 1;
    return 0;
  }
  if (op.size == 4)
  {
    uint16_t hi = (uint16_t)(w >> 16);
    uint16_t lo = (uint16_t)(w & 0xFFFF);
    /* Thumb-2 data-processing (modified immediate), Rd=PC encodes CMP/CMN/
     * TST/TEQ.  hi encoding: 1111 0i01 0xxx nnnn (op bits [24:21] = 0x4=TST,
     * 0x8=CMN, 0xD=CMP, 0x0=TST/AND-S — table varies; the canonical "no-write"
     * marker is lo[11:8] == 0xF (Rd = PC). */
    if ((hi & 0xFA00) == 0xF000 && (lo & 0x8000) == 0 && ((lo >> 8) & 0xF) == 0xF)
      return 1;
    /* Thumb-2 data-processing (plain binary immediate): same Rd=PC marker. */
    if ((hi & 0xFA00) == 0xF200 && (lo & 0x8000) == 0 && ((lo >> 8) & 0xF) == 0xF)
      return 1;
    /* Thumb-2 data-processing (shifted register): hi pattern 1110 101x xxxx
     * nnnn, lo[15] == 0, lo[11:8] == 0xF (Rd = PC) marks the flag-setter
     * variant (CMP.W reg, CMN.W reg, TST.W reg, TEQ.W reg). */
    if ((hi & 0xFE00) == 0xEA00 && (lo & 0x8000) == 0 && ((lo >> 8) & 0xF) == 0xF)
      return 1;
    return 0;
  }
  return 0;
}

static int thumb_decode_dest_reg(thumb_opcode op)
{
  uint32_t w = op.opcode;

  if (op.size == 2)
  {
    uint16_t hw = (uint16_t)(w & 0xFFFF);

    /* 16-bit shift-immediate / add / subtract: 000xx ... Rd3.  Covers
     * LSL/LSR/ASR(imm) and ADD/SUB(reg or imm3); every encoding writes the
     * low-register Rd in bits [2:0]. */
    if ((hw & 0xE000) == 0x0000)
      return hw & 0x07;

    /* 16-bit MOV/CMP/ADD/SUB (8-bit immediate): 001 op2 Rd3 imm8.
     * op2==01 is CMP (writes no GPR — leave to the flag-setter path);
     * MOV/ADD/SUB write Rd in bits [10:8]. */
    if ((hw & 0xE000) == 0x2000)
    {
      if (((hw >> 11) & 0x03) == 0x01)
        return -1;
      return (hw >> 8) & 0x07;
    }

    /* 16-bit data-processing (register): 010000 op4 Rm3 Rd3, Rd in bits [2:0].
     * TST(8), CMP(10), CMN(11) write no GPR. */
    if ((hw & 0xFC00) == 0x4000)
    {
      int op4 = (hw >> 6) & 0x0F;
      if (op4 == 0x8 || op4 == 0xA || op4 == 0xB)
        return -1;
      return hw & 0x07;
    }

    /* 16-bit MOV (high registers): 0100 0110 D Rm4 Rd3
     * Bits [15:8]=0x46, D=bit7 of lower byte, Rd3=bits[2:0] */
    if ((hw >> 8) == 0x46)
      return ((hw >> 4) & 0x08) | (hw & 0x07);
    /* 16-bit ADD (high registers): 0100 0100 D Rm4 Rd3 */
    if ((hw >> 8) == 0x44)
      return ((hw >> 4) & 0x08) | (hw & 0x07);
    /* 16-bit CMP (high registers) 0x45 and BX/BLX 0x47: no single-GPR dest. */

    /* 16-bit LDR (literal): 01001 Rt3 imm8, Rt in bits [10:8]. */
    if ((hw & 0xF800) == 0x4800)
      return (hw >> 8) & 0x07;

    /* 16-bit LDR (SP-relative): 1001 1 Rt3 imm8, Rt in bits [10:8].
     * (0x9000 is the STR form — no GPR dest.) */
    if ((hw & 0xF800) == 0x9800)
      return (hw >> 8) & 0x07;

    /* 16-bit ADR / ADD (SP plus immediate): 1010 x Rd3 imm8, Rd in bits [10:8]. */
    if ((hw & 0xF000) == 0xA000)
      return (hw >> 8) & 0x07;

    /* 16-bit sign/zero extend (SXTH/SXTB/UXTH/UXTB): 1011 0010 oo Rm3 Rd3. */
    if ((hw & 0xFF00) == 0xB200)
      return hw & 0x07;

    /* Remaining low-register and memory forms either don't write a single GPR
     * or are decoded by decode_str_ldr_imm before reaching here. */
    return -1;
  }

  if (op.size == 4)
  {
    uint16_t hi = (uint16_t)(w >> 16);
    uint16_t lo = (uint16_t)(w & 0xFFFF);
    /* Thumb-2 data-processing (modified immediate): 1111 0x0x xxxx xxxx | 0xxx xxxx xxxx xxxx
     * Rd = bits [11:8] of low halfword */
    if ((hi & 0xFA00) == 0xF000 && (lo & 0x8000) == 0)
      return (lo >> 8) & 0x0F;
    /* Thumb-2 data-processing (plain binary immediate): 1111 0x1x xxxx xxxx | 0xxx xxxx xxxx xxxx
     * Rd = bits [11:8] of low halfword */
    if ((hi & 0xFA00) == 0xF200 && (lo & 0x8000) == 0)
      return (lo >> 8) & 0x0F;
    /* Thumb-2 LDR/STR (immediate): 1111 1000 xxxx xxxx | xxxx xxxx xxxx xxxx
     * Rt = bits [15:12] of low halfword — for LDR, Rt is the dest */
    if ((hi & 0xFE00) == 0xF800)
    {
      int L = (hi >> 4) & 1; /* L=1 for loads */
      if (L)
        return (lo >> 12) & 0x0F;
    }
    /* Thumb-2 load word: 1111 1000 0101 xxxx | xxxx xxxx xxxx xxxx */
    if ((hi & 0xFFF0) == 0xF850)
      return (lo >> 12) & 0x0F;
    /* Thumb-2 MOVW/MOVT: 1111 0x10 x100 xxxx | 0xxx xxxx xxxx xxxx */
    if ((hi & 0xFBF0) == 0xF240 && (lo & 0x8000) == 0) /* MOVW */
      return (lo >> 8) & 0x0F;
    if ((hi & 0xFBF0) == 0xF2C0 && (lo & 0x8000) == 0) /* MOVT */
      return (lo >> 8) & 0x0F;
    /* Thumb-2 data-processing (shifted register): 1110 101x xxxx nnnn |
     * 0iii dddd iitt mmmm.  Rd = lo[11:8]; Rd==PC (0xF) marks the flag-setter
     * variant (CMP.W/CMN.W/TST.W/TEQ.W — no GPR write). */
    if ((hi & 0xFE00) == 0xEA00 && (lo & 0x8000) == 0)
    {
      int rd = (lo >> 8) & 0x0F;
      if (rd != 0x0F)
        return rd;
      return -1;
    }
  }

  return -1;
}

int ot(thumb_opcode op)
{
  if (op.size == 0)
    return op.size;

  /* Detect instructions that write to R9 when it's reserved for GOT pointer.
   * Exclude push/pop/stmdb/ldmia which legitimately save/restore R9. */
  if (text_and_data_separation && !allow_r9_write)
  {
    int dest = thumb_decode_dest_reg(op);
    if (dest == R9)
    {
      tcc_error("instruction 0x%0*x (size=%d) writes to R9 (GOT pointer) at ind=0x%x ir_op=%d", op.size == 4 ? 8 : 4,
                op.opcode, op.size, (unsigned)ind, g_debug_current_op);
    }
  }

  /* Update the MOV-coalescing register-equivalence cache and the STR->LDR
   * redundant-reload cache based on what is about to be emitted.  This only
   * tracks state — no elision happens here; elision is performed at the
   * call sites via ot_check_mov_reg / ot_check_ldr_imm so that ot()'s
   * return value remains the real emitted size and downstream jump/offset
   * accounting never sees a phantom emission.
   *
   * IT blocks: instructions inside an IT/ITx/ITxy/ITxyz are conditionally
   * executed.  Their writes are therefore not guaranteed, so destination
   * registers must be invalidated rather than recorded as equivalences. */
  if (thumb_gen_state.generating_function)
  {
    if (mov_equiv_it_pending > 0)
    {
      /* Conditional instruction: pessimistically drop anything this op
       * might write, and never record new equivalences.  Treat STR/LDR
       * the same way — their effect is gated on the IT condition. */
      int mv_rd = -1, mv_rm = -1;
      if (decode_mov_reg_plain(op, &mv_rd, &mv_rm))
      {
        mov_equiv_invalidate_reg(mv_rd);
        strldr_cache_invalidate_reg(mv_rd);
        imm_cache_invalidate_reg(mv_rd);
      }
      else if (thumb_op_is_pure_flag_setter(op))
      {
        /* CMP/CMN/TST/TEQ — no GPR clobber even under predication. */
      }
      else
      {
        int dest = thumb_decode_dest_reg(op);
        if (dest >= 0)
        {
          mov_equiv_invalidate_reg(dest);
          strldr_cache_invalidate_reg(dest);
          imm_cache_invalidate_reg(dest);
        }
        else
        {
          mov_equiv_reset_all();
          tcc_gen_machine_strldr_cache_reset();
          imm_cache_reset_all();
        }
      }
      mov_equiv_it_pending--;
    }
    else
    {
      int it_len = mov_equiv_it_block_length(op);
      if (it_len > 0)
      {
        /* IT itself writes no GPR; start the conditional window. */
        mov_equiv_it_pending = it_len;
      }
      else
      {
        int mv_rd = -1, mv_rm = -1;
        int sl_is_str = 0, sl_rt = 0, sl_rn = 0, sl_imm = 0;
        uint32_t sl_puw = 0;
        if (decode_str_ldr_imm(op, &sl_is_str, &sl_rt, &sl_rn, &sl_imm, &sl_puw))
        {
          if (sl_is_str)
          {
            /* STR does not write a register; record the store for
             * redundant-reload matching.  MOV-equiv is unaffected. */
            strldr_cache_record_str(sl_rt, sl_rn, sl_imm, sl_puw, op.size);
          }
          else
          {
            /* LDR writes Rt: invalidate both caches for that register.
             * If the call-site helper ran the match it would have
             * elided without reaching ot(); so if we get here, this LDR
             * is actually emitting and genuinely clobbers Rt. */
            mov_equiv_invalidate_reg(sl_rt);
            strldr_cache_invalidate_reg(sl_rt);
            imm_cache_invalidate_reg(sl_rt);
          }
        }
        else if (decode_mov_reg_plain(op, &mv_rd, &mv_rm))
        {
          mov_equiv_record_mov(mv_rd, mv_rm);
          strldr_cache_invalidate_reg(mv_rd);
          imm_cache_invalidate_reg(mv_rd);
        }
        else if (op.size == 4 &&
                 (((op.opcode >> 16) & 0xFE40) == 0xE840))
        {
          /* LDRD/STRD (Thumb-2): encoded as 1110 100P U1W0 nnnn (STRD) or
           * 1110 100P U1W1 nnnn (LDRD).  Bit 20 (high-halfword bit 4)
           * distinguishes load (1) vs store (0).
           *
           * STRD writes no GPR — only memory.  LDRD writes both Rt and Rt2
           * (low-halfword bits [15:12] and [11:8] respectively).  Either way
           * the rest of the GPR-equivalence cache is unaffected, so don't
           * fall through to the "unknown opcode → reset everything" path
           * which destroys upstream coalescing wins. */
          if ((op.opcode >> 20) & 1)
          {
            /* LDRD: invalidate Rt and Rt2 (writeback to Rn is rare here and
             * already covered by the writeback handling — for the typical
             * STRD imm with W=0 used by the codegen we don't touch Rn). */
            int rt = (int)((op.opcode >> 12) & 0xF);
            int rt2 = (int)((op.opcode >> 8) & 0xF);
            mov_equiv_invalidate_reg(rt);
            mov_equiv_invalidate_reg(rt2);
            strldr_cache_invalidate_reg(rt);
            strldr_cache_invalidate_reg(rt2);
            imm_cache_invalidate_reg(rt);
            imm_cache_invalidate_reg(rt2);
          }
          /* STRD: no GPR write, leave the mov_equiv cache alone. */
        }
        else if (thumb_op_is_pure_flag_setter(op))
        {
          /* CMP/CMN/TST/TEQ write only the flags — no GPR clobber, no
           * cache invalidation needed. */
        }
        else
        {
          int dest = thumb_decode_dest_reg(op);
          if (dest >= 0)
          {
            mov_equiv_invalidate_reg(dest);
            strldr_cache_invalidate_reg(dest);
            imm_cache_invalidate_reg(dest);
          }
          else
          {
            mov_equiv_reset_all();
            tcc_gen_machine_strldr_cache_reset();
            imm_cache_reset_all();
          }
        }
      }
    }
  }
  else
  {
    mov_equiv_reset_all();
    tcc_gen_machine_strldr_cache_reset();
    imm_cache_reset_all();
  }

  /* Dry run: don't emit actual opcodes, but still track code size and
   * handle literal pool generation to ensure code addresses match real pass. */
  if (dry_run_state.active)
  {
    if (thumb_gen_state.generating_function)
    {
      thumb_gen_state.code_size += op.size;
      /* Check if literal pool needs to be generated during dry-run.
       * We need to call th_literal_pool_generate to properly track the
       * code size including the literal pool, so that ind matches
       * between dry-run and real pass. */
      const int max_offset = thumb_gen_state.code_size + thumb_gen_state.literal_pool_count * 4;
      if (max_offset >= 1020)
      {
        th_literal_pool_generate();
      }
    }
    /* Increment ind as if we emitted the instruction, but don't write to section */
    ind += op.size;
    return op.size;
  }

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

static void gcall_or_jump_mop(int is_jmp, MachineOperand target);

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
  else if ((xa & 0xf500) == 0xb100)
  {
    /* CBZ/CBNZ encoding: offset = (i:imm5) * 2, forward only */
    uint32_t i_bit = (xa >> 9) & 1;
    uint32_t imm5 = (xa >> 3) & 0x1f;
    uint32_t imm6 = (i_bit << 5) | imm5;
    xa = (int)(imm6 * 2) + pos + 4;
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
    return th_mvn_imm(r, 0, -imm - 1, flags_safe(), ENFORCE_ENCODING_NONE);
  }
  return th_mov_imm(r, imm, flags_safe(), ENFORCE_ENCODING_NONE);
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
    load_full_const(rr, PREG_NONE, LFC_SPLIT(sign ? -off : off));
    return alloc;
  }

  if (sign)
    ot_check(th_rsb_imm(rr, rr, 0, flags_safe(), ENFORCE_ENCODING_NONE));
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
  else if ((*x & 0xf500) == 0xb100)
  {
    /* CBZ/CBNZ: 16-bit, forward-only, range 0-126 bytes.
     * CBZ base = 0xb100, CBNZ base = 0xb900; both match (x & 0xf500) == 0xb100
     * since bit 11 (0x0800) is not in the mask.
     * Encoding: op | (i << 9) | (imm5 << 3) | Rn
     * where offset = (i:imm5) * 2 */
    int offset = a - (lt + 4); /* PC-relative, Thumb PC = insn + 4 */
    if (offset < 0 || offset > 126 || (offset & 1))
      tcc_error("compiler_error: CBZ/CBNZ target out of range: offset=%d", offset);
    uint32_t imm6 = (uint32_t)offset >> 1;
    uint32_t i_bit = (imm6 >> 5) & 1;
    uint32_t imm5 = imm6 & 0x1f;
    *x &= 0xfd07; /* Keep base opcode, NZ bit, and Rn */
    *x |= (uint16_t)((i_bit << 9) | (imm5 << 3));
  }
  else
    tcc_error("compiler_error: unhandled branch type in th_patch_call for: t: "
              "0x%x, a: 0x%x, x: 0x%x 0x%x\n",
              t, a, x[0], x[1]);

  return t;
}

/* Add a value to SP.  When the immediate doesn't fit the ADD/SUB SP encoding,
 * a scratch register is needed.  scratch_reg selects which one:
 *   >= 0  : use that specific physical register (caller guarantees it's free)
 *   < 0   : default to R_IP (safe in prologue/epilogue where R0-R3 hold args)
 */
static void gadd_sp_ex(int val, int scratch_reg)
{
  if (val == 0)
    return;

  if (scratch_reg < 0)
    scratch_reg = R_IP;

  if (val > 0)
  {
    thumb_opcode add_imm = th_add_imm(R_SP, R_SP, (uint32_t)val, flags_safe(), ENFORCE_ENCODING_NONE);
    if (is_valid_opcode(add_imm))
    {
      ot(add_imm);
      return;
    }

    load_full_const(scratch_reg, PREG_NONE, (uint32_t)val, 0);
    ot_check(
        th_add_reg(R_SP, R_SP, scratch_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    return;
  }

  /* val < 0 */
  const uint32_t sub = (uint32_t)(-val);
  thumb_opcode sub_imm = th_sub_imm(R_SP, R_SP, sub, flags_safe(), ENFORCE_ENCODING_NONE);
  if (is_valid_opcode(sub_imm))
  {
    ot(sub_imm);
    return;
  }

  load_full_const(scratch_reg, PREG_NONE, (uint32_t)sub, 0);
  ot_check(th_sub_reg(R_SP, R_SP, scratch_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
}

static void gadd_sp(int val)
{
  gadd_sp_ex(val, -1);
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

ST_FUNC void tcc_gen_machine_indirect_jump_mop(MachineOperand src, TccIrOp op)
{
  (void)op;
  MachineCodegenContext ctx = {0};
  int target = mach_ensure_in_reg(&ctx, &src, 0);
  ot_check(th_bx_reg((uint16_t)target));
  mach_release_all(&ctx);
}

/* Returns the number of bytes emitted by tcc_gen_machine_switch_table_mop for
 * a table with the given number of entries.  Used by the dry-run pass in
 * codegen.c so that branch-offset analysis is accurate without the backend
 * having to emit any real instructions. */
ST_FUNC int tcc_gen_machine_switch_table_dry_run_size(int num_entries)
{
  /* Layout: LSL.W(4) + ADD(2) + LDR.W(4) + ADD(2) + BX(2) = 14 bytes preamble
   * + 4 bytes per table entry (32-bit signed PC-relative offsets). */
  return 14 + num_entries * 4;
}

/* Force any pending literal pool to be flushed before a region of
 * `upcoming_bytes` is emitted, if leaving the pool pending that long would
 * push its load out of range.  Public wrapper so codegen.c can reserve
 * space symmetrically in both the dry-run and real-run passes.
 *
 * The SWITCH_TABLE dispatch needs this: its preamble (LSL/ADD/LDR/ADD/BX)
 * must be emitted atomically — a literal-pool flush in the middle relocates
 * the terminal `ADD Rt, PC; BX Rt` past the pool (bridged by a B.W), which
 * invalidates the `ref_point == table_start` assumption that the switch-
 * table offset backpatch in codegen.c relies on, producing a wild jump.
 * Flushing the pool up front (in both passes, so dry-run size estimates and
 * real-run addresses stay consistent) keeps the preamble + table contiguous. */
ST_FUNC void tcc_gen_machine_reserve_pool_bytes(int upcoming_bytes)
{
  th_literal_pool_reserve_upcoming_bytes(upcoming_bytes);
}

/* MOP variant: accepts a MachineOperand for the index register. */
ST_FUNC void tcc_gen_machine_switch_table_mop(MachineOperand src, TCCIRSwitchTable *table, TCCIRState *ir, int ir_idx)
{
  (void)ir_idx;

  TRACE("'tcc_gen_machine_switch_table_mop' table_id=%d entries=%d\n", table - ir->switch_tables, table->num_entries);

  MachineCodegenContext ctx = {0};
  /* The index value must be in a register at this point. */
  int index_reg = mach_ensure_in_reg(&ctx, &src, 0);
  if (!thumb_is_hw_reg(index_reg))
    tcc_error("internal error: SWITCH_TABLE index not in a hardware register (mop)");

  /* Use R_IP as scratch to avoid clobbering index_reg, which may still be
     live at the switch targets (SSA can place the loop counter directly here). */
  int rt = R_IP;

  ot_check(th_lsl_imm(rt, index_reg, 2, flags_safe(), ENFORCE_ENCODING_32BIT));
  ot_check(th_add_reg(rt, rt, R_PC, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
  ot_check_ldr_imm(rt, rt, 6, 6, ENFORCE_ENCODING_32BIT);
  ot_check(th_add_reg(rt, rt, R_PC, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
  ot_check(th_bx_reg(rt));

  int table_start = ind;
  for (int i = 0; i < table->num_entries; i++)
  {
    g(0);
    g(0);
    g(0);
    g(0);
  }
  table->table_code_addr = table_start;
  mach_release_all(&ctx);
}

/* SWITCH_LOAD: data-table dispatch that loads values[index] into dest.
 *
 * Layout (uniform 14-byte preamble):
 *
 *   LSL.W rt, index, #2          (4 bytes)
 *   ADD   rt, rt, pc             (2 bytes)         ; PC=preamble_start+8
 *   LDR.W ip,  [rt, #6]          (4 bytes)         ; load table[index] -> ip
 *   B.W   skip                   (4 bytes)         ; jump past the table
 *   <table data>                 (4*N bytes)
 *   skip:
 *   [optional STR/MOV ip -> dest]                  ; only if dest is spilled,
 *                                                  ;   emitted by the IR-level
 *                                                  ;   ASSIGN that follows.
 *
 * The fixed loaded register is R_IP (same as SWITCH_TABLE's scratch); the
 * IR-level optimization wraps SWITCH_LOAD with an ASSIGN that places IP into
 * the real dest, so we don't need a separate spill path here.
 *
 * SYMREF entries emit R_ARM_ABS32 relocations at their table slots; the
 * linker fills in the absolute symbol address.
 */
/* SWITCH_LOAD dispatch size: literal-pool LDR (4 bytes, T2 encoding for
 * R_IP) + indexed shifted LDR.W (4 bytes).  The table itself lives in
 * .rodata and contributes no .text bytes. */
ST_FUNC int tcc_gen_machine_switch_load_dry_run_size(int num_entries)
{
  (void)num_entries;
  return 8;
}

ST_FUNC void tcc_gen_machine_switch_load_mop(MachineOperand src, MachineOperand dest, TCCIRSwitchValueTable *vtab,
                                             TCCIRState *ir, int ir_idx)
{
  (void)ir_idx;
  (void)ir;

  TRACE("'tcc_gen_machine_switch_load_mop' vt_id=%d entries=%d\n", (int)(vtab - ir->switch_value_tables),
        vtab->num_entries);

  if (!vtab->rodata_sym)
    tcc_error("internal error: SWITCH_LOAD table has no rodata symbol (switch_to_data should have allocated it)");

  MachineCodegenContext ctx = {0};
  /* Keep the index out of R_IP, which we clobber with the table base below. */
  int index_reg = mach_ensure_in_reg(&ctx, &src, (1u << (uint32_t)R_IP));
  if (!thumb_is_hw_reg(index_reg))
    tcc_error("internal error: SWITCH_LOAD index not in a hardware register");

  /* Resolve the destination register.  The switch_to_data optimization tries to
   * keep the SWITCH_LOAD dest in a hardware register, but under high register
   * pressure the allocator can spill it (or it may be an lvalue store).  Rather
   * than bail out, allocate a scratch via mach_get_dest_reg() and store it back
   * with mach_writeback_dest() afterwards.  Exclude index_reg and R_IP — both
   * are read by the indexed load below. */
  uint32_t dest_excl = (1u << (uint32_t)index_reg) | (1u << (uint32_t)R_IP);
  int dest_reg = mach_get_dest_reg(&ctx, &dest, dest_excl);

  /* Load the table's base address from the literal pool into IP. */
  _lfc_sym = vtab->rodata_sym;
  load_full_const(R_IP, PREG_NONE, 0, 0);

  /* dest = table[index] via LDR.W dest, [ip, index, LSL #2]. */
  thumb_shift shift = {THUMB_SHIFT_LSL, 2, THUMB_SHIFT_IMMEDIATE};
  ot_check(th_ldr_reg((uint32_t)dest_reg, (uint32_t)R_IP, (uint32_t)index_reg, shift, ENFORCE_ENCODING_32BIT));

  /* If the dest was a spill slot or lvalue, write the loaded value back. */
  mach_writeback_dest(&dest, dest_reg);

  mach_release_all(&ctx);
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
  ot_check(th_sub_reg(r, R_SP, r, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));

  if (align < 8)
    align = 8;
  if (align & (align - 1))
    tcc_error("alignment is not a power of 2: %i", align);

  if (align > 1)
  {
    /* Try immediate BIC first; if it doesn't encode, fall back to register mask. */
    if (!ot(th_bic_imm(r, r, (uint32_t)(align - 1), flags_safe(), ENFORCE_ENCODING_NONE)))
    {
      ScratchRegAlloc mask_alloc = get_scratch_reg_with_save(1u << r);
      int mask_reg = mask_alloc.reg;
      if (!ot(th_generic_mov_imm(mask_reg, align - 1)))
      {
        load_full_const(mask_reg, PREG_NONE, LFC_SPLIT(align - 1));
      }
      ot_check(th_bic_reg(r, r, mask_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      if (mask_alloc.saved)
      {
        ot_check(th_pop(1u << mask_reg));
      }
    }
  }

  /* SP = r */
  ot_check_mov_reg(R_SP, r, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);

  vpop();
}

ST_FUNC void gen_vla_sp_save(int addr)
{
  if (nocode_wanted)
    return;

  /* Store SP to the local stack slot at frame offset `addr`. */
  int off = fp_adjust_local_offset(addr, 0 /* not param */);
  int sign = (off < 0) ? 1 : 0;
  int abs_off = sign ? -off : off;
  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;

  ScratchRegAlloc vla_sc = get_scratch_reg_with_save(0);
  ot_check_mov_reg(vla_sc.reg, R_SP, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
  th_store32_imm_or_reg_ex(vla_sc.reg, base_reg, abs_off, sign, 0);
  restore_scratch_reg(&vla_sc);
}

ST_FUNC void gen_vla_sp_restore(int addr)
{
  if (nocode_wanted)
    return;

  /* Load SP from the local stack slot at frame offset `addr`. */
  int off = fp_adjust_local_offset(addr, 0 /* not param */);
  int sign = (off < 0) ? 1 : 0;
  int abs_off = sign ? -off : off;
  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;

  ScratchRegAlloc vla_sc = get_scratch_reg_with_save(0);
  load_from_base(vla_sc.reg, PREG_REG_NONE, IROP_BTYPE_INT32, 0, abs_off, sign, base_reg);
  ot_check_mov_reg(R_SP, vla_sc.reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
  restore_scratch_reg(&vla_sc);
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

/* Returns 1 if a 64-bit access at (sym + addend) is guaranteed 4-byte aligned
 * (so LDRD/STRD is safe).  Conservative: only allows natural alignment for
 * non-struct, non-packed symbols, plus any explicit alignment >= 4. */
static int sym_is_4_byte_aligned_for_64bit(Sym *sym, int32_t addend)
{
  if (!sym)
    return 0;
  if ((addend & 3) != 0)
    return 0;
  if (sym->a.packed)
    return 0;
  if (sym->a.aligned >= 3) /* explicit alignment 2^(n-1) >= 4 */
    return 1;
  if (sym->a.aligned > 0) /* explicit 1 or 2 byte alignment — not safe */
    return 0;
  /* sym->a.aligned == 0: rely on the declared type's natural alignment.
   * Structs/unions may be packed-wrapped; reject conservatively.  Native
   * scalars (long long, double, pointer) have natural alignment >= 4. */
  int btype = sym->type.t & VT_BTYPE;
  if (btype == VT_STRUCT)
    return 0;
  return 1;
}

/* Try to emit STRD Rt, Rt2, [base, #±abs_off] for a 64-bit paired store.
 * Constraints (Thumb-2 STRD imm T1):
 *   - Rt != Rt2
 *   - Rt, Rt2 in r0..r12 or r14 (not SP, not PC)
 *   - abs_off 4-byte aligned and <= 1020
 * Returns 1 on success, 0 if the caller must fall back to two 32-bit stores. */
static int try_strd_pair(int lo_reg, int hi_reg, int base, int abs_off, int sign)
{
  if ((abs_off & 3) != 0 || abs_off > 1020)
    return 0;
  if (lo_reg < 0 || lo_reg > R_LR || lo_reg == R_SP)
    return 0;
  if (hi_reg < 0 || hi_reg > R_LR || hi_reg == R_SP)
    return 0;
  const uint32_t puw = sign ? 4u : 6u;
  ot_check(th_strd_imm((uint32_t)lo_reg, (uint32_t)hi_reg, (uint32_t)base, abs_off, puw));
  return 1;
}

/* Mirror of try_strd_pair for LDRD.  Same register and offset constraints;
 * the caller is responsible for guaranteeing 4-byte alignment of the target
 * address (stack, or a symbol that passes sym_is_4_byte_aligned_for_64bit). */
static int try_ldrd_pair(int lo_reg, int hi_reg, int base, int abs_off, int sign)
{
  if ((abs_off & 3) != 0 || abs_off > 1020)
    return 0;
  if (lo_reg < 0 || lo_reg > R_LR || lo_reg == R_SP)
    return 0;
  if (hi_reg < 0 || hi_reg > R_LR || hi_reg == R_SP)
    return 0;
  if (lo_reg == hi_reg)
    return 0;
  const uint32_t puw = sign ? 4u : 6u;
  ot_check(th_ldrd_imm((uint32_t)lo_reg, (uint32_t)hi_reg, (uint32_t)base, abs_off, puw));
  return 1;
}

/* Emit a single STR to a spill slot. Used by the codegen STRD pairing logic
 * to flush a pending store when pairing wasn't possible. */
ST_FUNC void tcc_gen_machine_store_spill(int src_reg, int32_t spill_offset)
{
  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
  int adj = fp_adjust_local_offset(spill_offset, 0);
  int sign = (adj < 0);
  int abs_off = sign ? -adj : adj;
  ot_check_str_imm((uint32_t)src_reg, (uint32_t)base_reg,
                   abs_off, sign ? 4u : 6u, ENFORCE_ENCODING_NONE);
}

/* Try to emit STRD for two 32-bit values to adjacent spill slots.
 * off1 must be the lower offset (off1 + 4 == off2).
 * Returns 1 on success, 0 if STRD constraints not met. */
ST_FUNC int tcc_gen_machine_try_strd_spill(int reg1, int32_t off1, int reg2, int32_t off2)
{
  if (off1 + 4 != off2)
    return 0;
  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
  int adj = fp_adjust_local_offset(off1, 0);
  int sign = (adj < 0);
  int abs_off = sign ? -adj : adj;
  return try_strd_pair(reg1, reg2, base_reg, abs_off, sign);
}

/* Try to emit LDRD for two 32-bit values from adjacent spill slots.
 * off1 must be the lower offset (off1 + 4 == off2).
 * Returns 1 on success, 0 if LDRD constraints not met. */
ST_FUNC int tcc_gen_machine_try_ldrd_spill(int reg1, int32_t off1, int reg2, int32_t off2)
{
  if (off1 + 4 != off2)
    return 0;
  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
  int adj = fp_adjust_local_offset(off1, 0);
  int sign = (adj < 0);
  int abs_off = sign ? -adj : adj;
  return try_ldrd_pair(reg1, reg2, base_reg, abs_off, sign);
}

/* Try to emit LDRD/STRD for two 32-bit values from adjacent offsets off a
 * generic base register (not FP/SP).  Used by the LOAD_INDEXED/STORE_INDEXED
 * pairing peephole.  `off` is the lower offset (caller has verified
 * off + 4 fits within the same access range).  Returns 1 on success. */
ST_FUNC int tcc_gen_machine_try_ldrd_base(int reg1, int reg2, int base_reg, int32_t off)
{
  int sign = (off < 0);
  int abs_off = sign ? -off : off;
  return try_ldrd_pair(reg1, reg2, base_reg, abs_off, sign);
}

ST_FUNC int tcc_gen_machine_try_strd_base(int reg1, int reg2, int base_reg, int32_t off)
{
  int sign = (off < 0);
  int abs_off = sign ? -off : off;
  return try_strd_pair(reg1, reg2, base_reg, abs_off, sign);
}

ST_FUNC int tcc_gen_machine_try_strd_imm_spill(int64_t val1, int64_t val2,
                                               int32_t off1, int32_t off2)
{
  if (off1 + 4 != off2)
    return 0;
  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
  int adj = fp_adjust_local_offset(off1, 0);
  int sign = (adj < 0);
  int abs_off = sign ? -adj : adj;
  if ((abs_off & 3) != 0 || abs_off > 1020)
    return 0;

  MachineCodegenContext ctx = {0};
  MachineOperand op1 = {.kind = MACH_OP_IMM, .u.imm.val = val1};
  int r1 = mach_ensure_in_reg(&ctx, &op1, 0);
  int r2;
  if (val1 == val2) {
    r2 = r1;
  } else {
    MachineOperand op2 = {.kind = MACH_OP_IMM, .u.imm.val = val2};
    r2 = mach_ensure_in_reg(&ctx, &op2, (1u << (uint32_t)r1));
  }
  if (r1 == R_SP || r2 == R_SP) {
    mach_release_all(&ctx);
    return 0;
  }
  const uint32_t puw = sign ? 4u : 6u;
  ot_check(th_strd_imm((uint32_t)r1, (uint32_t)r2, (uint32_t)base_reg, abs_off, puw));
  mach_release_all(&ctx);
  return 1;
}

ST_FUNC int tcc_gen_machine_try_strd_imm_base(int64_t val1, int64_t val2,
                                              int base_reg, int32_t off)
{
  int sign = (off < 0);
  int abs_off = sign ? -off : off;
  if ((abs_off & 3) != 0 || abs_off > 1020)
    return 0;

  uint32_t excl = (1u << (uint32_t)base_reg);
  MachineCodegenContext ctx = {0};
  MachineOperand op1 = {.kind = MACH_OP_IMM, .u.imm.val = val1};
  int r1 = mach_ensure_in_reg(&ctx, &op1, excl);
  int r2;
  if (val1 == val2) {
    r2 = r1;
  } else {
    MachineOperand op2 = {.kind = MACH_OP_IMM, .u.imm.val = val2};
    r2 = mach_ensure_in_reg(&ctx, &op2, excl | (1u << (uint32_t)r1));
  }
  if (r1 == R_SP || r2 == R_SP) {
    mach_release_all(&ctx);
    return 0;
  }
  const uint32_t puw = sign ? 4u : 6u;
  ot_check(th_strd_imm((uint32_t)r1, (uint32_t)r2, (uint32_t)base_reg, abs_off, puw));
  mach_release_all(&ctx);
  return 1;
}

ST_FUNC int tcc_machine_can_encode_stack_offset_for_reg(int frame_offset, int dest_reg)
{
  /* Check if frame_offset can be directly encoded in ldr/str instructions
   * without requiring a scratch register. This is used to avoid wasteful
   * address materialization when the backend can handle the offset directly.
   * Tests with dest_reg since encoding availability depends on the register. */
  /* Adjust for callee-saved gap below FP (spill offsets are always locals) */
  frame_offset = fp_adjust_local_offset(frame_offset, 0);
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

  /* Adjust for callee-saved gap below FP (spill slots are always locals) */
  frame_offset = fp_adjust_local_offset(frame_offset, 0);

  /* Peephole: if the previous emit was a STR or LDR of the same register to/from
   * the same slot AND no other instruction has been emitted since, the value is
   * already in dest_reg — skip the redundant load. */
  TCCIRState *ir = tcc_state ? tcc_state->ir : NULL;
  if (ir && ir->spill_cache.last_emit_kind != 0 &&
      ir->spill_cache.last_emit_ind == ind &&
      ir->spill_cache.last_emit_reg == dest_reg &&
      ir->spill_cache.last_emit_offset == frame_offset)
  {
    return;
  }

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

  if (ir)
  {
    ir->spill_cache.last_emit_kind = 2; /* LDR */
    ir->spill_cache.last_emit_ind = ind;
    ir->spill_cache.last_emit_reg = (int8_t)dest_reg;
    ir->spill_cache.last_emit_offset = frame_offset;
  }
}

ST_FUNC void tcc_machine_store_spill_slot(int src_reg, int frame_offset)
{
  if (src_reg == PREG_REG_NONE)
    tcc_error("compiler_error: store_spill_slot requires a source register");

  /* Adjust for callee-saved gap below FP (spill slots are always locals) */
  frame_offset = fp_adjust_local_offset(frame_offset, 0);
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

  TCCIRState *ir = tcc_state ? tcc_state->ir : NULL;
  if (ir)
  {
    ir->spill_cache.last_emit_kind = 1; /* STR */
    ir->spill_cache.last_emit_ind = ind;
    ir->spill_cache.last_emit_reg = (int8_t)src_reg;
    ir->spill_cache.last_emit_offset = frame_offset;
  }
}

/* Like tcc_machine_store_spill_slot, but for stack-passed parameters.
 * Adds offset_to_args (prologue push size) to the frame offset so that
 * the store targets the correct caller-stack location above FP. */
ST_FUNC void tcc_machine_store_param_slot(int src_reg, int frame_offset)
{
  tcc_machine_store_spill_slot(src_reg, frame_offset + offset_to_args);
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

static ThumbLiteralPoolEntry *th_literal_pool_allocate()
{
  ThumbLiteralPoolEntry *entry;

  /* During dry-run, use separate pool to avoid modifying the real pool.
   * This prevents memory corruption when restoring state after dry-run. */
  if (dry_run_state.active)
  {
    if (dry_run_literal_pool_count >= dry_run_literal_pool_size)
    {
      dry_run_literal_pool_size <<= 1;
      dry_run_literal_pool =
          tcc_realloc(dry_run_literal_pool, dry_run_literal_pool_size * sizeof(ThumbLiteralPoolEntry));
      tcc_chained_hash_reserve(&literal_pool_hash, dry_run_literal_pool_size);
    }
    entry = &dry_run_literal_pool[dry_run_literal_pool_count++];
    entry->sym = NULL;
    entry->relocation = -1;
    entry->shared_index = -1;
    /* Track the count in the main state for code size calculations */
    thumb_gen_state.literal_pool_count++;
    return entry;
  }

  if (thumb_gen_state.literal_pool_count >= thumb_gen_state.literal_pool_size)
  {
    const int new_size = thumb_gen_state.literal_pool_size << 1;
    thumb_gen_state.literal_pool = tcc_realloc(thumb_gen_state.literal_pool, new_size * sizeof(ThumbLiteralPoolEntry));
    thumb_gen_state.literal_pool_size = new_size;
    tcc_chained_hash_reserve(&literal_pool_hash, new_size);
  }
  entry = &thumb_gen_state.literal_pool[thumb_gen_state.literal_pool_count++];
  entry->sym = NULL;
  entry->relocation = -1;
  entry->shared_index = -1;
  return entry;
}

/* Find existing literal pool entry with same sym and imm, and allocate new
   entry that shares its literal value.
   Uses hash table for O(1) lookup instead of O(n) linear search. */
static ThumbLiteralPoolEntry *th_literal_pool_find_or_allocate(Sym *sym, int64_t imm)
{
  int found_index;
  uint32_t full_hash;
  TCCChainedHash *hash;
  LiteralPoolLookupCache *cache;
  ThumbLiteralPoolEntry *pool;
  int new_index;

  if (dry_run_state.active)
  {
    hash = &literal_pool_hash;
    cache = &literal_pool_last_lookup;
    pool = dry_run_literal_pool;
    new_index = dry_run_literal_pool_count;
  }
  else
  {
    hash = &literal_pool_hash;
    cache = &literal_pool_last_lookup;
    pool = thumb_gen_state.literal_pool;
    new_index = thumb_gen_state.literal_pool_count;
  }

  full_hash = literal_pool_hash_func(sym, imm);
  found_index = literal_pool_lookup_cache_find(cache, full_hash, sym, imm);
  if (found_index < 0)
  {
    found_index = literal_pool_hash_find(hash, pool, full_hash, sym, imm);
  }

  /* Allocate new entry */
  ThumbLiteralPoolEntry *entry = th_literal_pool_allocate();
  if (found_index >= 0)
  {
    /* Mark as sharing with the found entry */
    entry->shared_index = found_index;
  }
  else
  {
    literal_pool_hash_insert(hash, full_hash, new_index);
    found_index = new_index;
  }
  literal_pool_lookup_cache_insert(cache, full_hash, sym, imm, found_index);
  return entry;
}

static void load_full_const(int r, int r1, uint32_t imm_lo, uint32_t imm_hi)
{
  struct Sym *sym = _lfc_sym;
  _lfc_sym = NULL;
  int64_t imm = (int64_t)((uint64_t)imm_hi << 32 | (uint64_t)imm_lo);
  ThumbLiteralPoolEntry *entry;
  thumb_opcode load_ins;
  int patch_pos;

  /* Validate symbol - only use symbols that can be externalized */
  sym = validate_sym_for_reloc(sym);

  /* Stable cache key: the validated symbol *before* the registration block
   * below may NULL it.  Registration is skipped during dry-run, so using the
   * post-registration `sym` would make the dry and real passes disagree on
   * cache hits and desynchronise code size.  `reuse_sym` is identical in both
   * passes (validate_sym_for_reloc does not depend on dry-run state). */
  Sym *reuse_sym = sym;

  /* Symbol-address reuse: when a register already holds &sym+imm, skip the
   * redundant literal-pool load.  Uses the same per-register imm_cache that
   * is invalidated on every clobbering emit and at IR boundaries, so the
   * decision is deterministic across the dry-run and real passes.  Only the
   * single-register (non-LDRD) form participates. */
  if (reuse_sym && thumb_gen_state.generating_function && r1 == PREG_NONE && r >= 0 && r < 16)
  {
    if (imm_cache[r].valid && imm_cache[r].sym == reuse_sym && imm_cache[r].value == imm)
      return; /* r already holds &sym+imm */
    for (int rr = 0; rr < 16; rr++)
    {
      if (rr != r && imm_cache[rr].valid && imm_cache[rr].sym == reuse_sym && imm_cache[rr].value == imm)
      {
        /* Another register holds it: copy instead of reloading from the
         * literal pool (saves a memory access and a pool word). */
        ot_check_mov_reg((uint32_t)r, (uint32_t)rr, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
        imm_cache[r].value = imm;
        imm_cache[r].sym = reuse_sym;
        imm_cache[r].valid = 1;
        return;
      }
    }
  }

  /* During dry-run, skip symbol registration and literal pool allocation.
   * We just emit the instruction (ot_check handles dry-run mode) to track
   * code size and scratch register usage, without creating side effects. */
  if (!dry_run_state.active)
  {
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
    load_ins = th_ldrd_imm(r, r1, R_PC, 0, 4);
  }
  ot_check(load_ins);
  patch_pos = ind - load_ins.size;

  /* Record that r now holds &sym+imm so a later reference to the same global
   * address can be elided.  Must run after ot_check(), whose emit-level
   * invalidation cleared imm_cache[r] for the LDR we just produced.  Keyed on
   * the pre-registration `reuse_sym` for dry/real-pass consistency. */
  if (reuse_sym && thumb_gen_state.generating_function && r1 == PREG_NONE && r >= 0 && r < 16)
  {
    imm_cache[r].value = imm;
    imm_cache[r].sym = reuse_sym;
    imm_cache[r].valid = 1;
  }

  /* During dry-run, we still need to create the literal pool entry to ensure
   * the literal pool behavior (threshold checks, sharing, etc.) matches the real pass.
   * We still set sym so that find_or_allocate can match entries correctly.
   * We just skip symbol registration and relocation setup. */
  entry = th_literal_pool_find_or_allocate(sym, imm);
  entry->sym = sym;
  entry->patch_position = patch_pos;
  entry->relocation = -1; /* No relocation by default */
  entry->data_size = (r1 == PREG_NONE) ? 4 : 8;
  entry->short_instruction = (r1 == PREG_NONE && load_ins.size == 2);

  if (!sym)
  {
    entry->imm = imm;
    return;
  }

  /* Re-derive esym after ot_check(): literal pool generation during ot_check
   * can call put_elf_sym → section_ptr_add → section_realloc, which may
   * free and reallocate the symtab section buffer, invalidating any
   * earlier ElfSym pointer. */
  ElfSym *esym = elfsym(sym);
  int sym_off = 0;
  if (esym)
  {
    sym_off = esym->st_shndx;
  }
  if (!pic)
  {
    entry->relocation = R_ARM_ABS32;
    /* The imm value is the addend (offset from symbol base).
       For arr[i], imm = i * sizeof(element).
       The linker will add the symbol's address to this addend. */
    entry->imm = imm;
  }
  else
  {
    /* For PIC without a symbol, the literal is a plain constant (e.g. -1).
     * Must still store the value so the pool emits it correctly. */
    entry->imm = imm;
    if (sym)
    {
      if (text_and_data_separation)
      {
        /* Relocation strategy for text_and_data_separation + PIC:
         *
         * R_ARM_GOTOFF computes (symbol - GOT_addr) and at runtime adds R9.
         * This only works when symbol and GOT are in the same loadable
         * segment (i.e. both in data).  With text/data separation, code
         * (.text) and data (.got) are loaded at independent addresses.
         *
         * Static symbols in *data* sections (no SHF_EXECINSTR):
         *   GOTOFF is fine — symbol and GOT are both in the data segment.
         *
         * Everything else (including static functions in other .text.*
         * sections from -ffunction-sections):
         *   Use R_ARM_GOT32 — indirect through a GOT slot.  The linker
         *   creates a GOT entry (put_got_entry → R_RELATIVE for locals),
         *   fill_local_got_entries writes sym->st_value into the slot,
         *   and the YAFF writer emits a data relocation so the dynamic
         *   loader patches the slot to the runtime code address.
         */
        int sym_in_code_section = 0;
        if (sym_off > 0 && sym_off < tcc_state->nb_sections)
        {
          Section *sym_sec = tcc_state->sections[sym_off];
          if (sym_sec && (sym_sec->sh_flags & SHF_EXECINSTR))
            sym_in_code_section = 1;
        }
        if (sym->type.t & VT_STATIC && sym_off != SHN_UNDEF && sym_off != cur_text_section->sh_num &&
            !sym_in_code_section)
        {
          /* Static data symbol — GOTOFF (same segment as GOT).
           * sym_off == SHN_UNDEF means the function is forward-declared
           * but not yet defined — we don't know its section, so we must
           * use GOT32 (safe indirect path) instead of GOTOFF. */
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
        /* Mirror the relocation selection above:
         * - Static data symbol → R_ARM_GOTOFF → add R9
         * - Everything else    → R_ARM_GOT32  → add R9; ldr [r]; add imm
         */
        int sym_in_code_section_cg = 0;
        if (sym_off > 0 && sym_off < tcc_state->nb_sections)
        {
          Section *sym_sec = tcc_state->sections[sym_off];
          if (sym_sec && (sym_sec->sh_flags & SHF_EXECINSTR))
            sym_in_code_section_cg = 1;
        }
        if (sym->type.t & VT_STATIC && sym_off != SHN_UNDEF && sym_off != cur_text_section->sh_num &&
            !sym_in_code_section_cg)
        {
          /* Static data symbol — GOTOFF (add R9) */
          ot_check(th_add_reg(r, r, R9, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        }
        else
        {
          thumb_opcode ot;
          ot_check(th_add_reg(r, r, R9, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));

          ot_check_ldr_imm(r, r, 0, 6, ENFORCE_ENCODING_NONE);
          ot = th_add_imm(r, r, imm, flags_safe(), ENFORCE_ENCODING_NONE);
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
                th_add_reg(r, r, scratch, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
            restore_scratch_reg(&scratch_alloc);
          }
        }
      }
      else
      {
        if (sym->type.t & VT_STATIC)
        {
          ot_check(th_add_reg(r, r, R_PC, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          ot_check(th_sub_imm(r, r, 8, flags_safe(), ENFORCE_ENCODING_NONE));
        }
        else
        {
          thumb_opcode ot;
          ot_check(th_add_reg(r, r, R_PC, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          ot_check_ldr_imm(r, r, 4, 6, ENFORCE_ENCODING_NONE);
          ot = th_add_imm(r, r, imm, flags_safe(), ENFORCE_ENCODING_NONE);
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
                th_add_reg(r, r, scratch, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
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
   * When computing their address, fold in offset_to_args (prologue push size).
   * Locals/spills need callee-saved gap adjustment. */
  if (is_param)
    frame_offset += offset_to_args;
  else
    frame_offset = fp_adjust_local_offset(frame_offset, 0);

  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;

  if (frame_offset == 0)
  {
    if (dest_reg != base_reg)
    {
      ot_check_mov_reg(dest_reg, base_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                       false);
    }
    return;
  }

  /* Check FP offset cache for existing computation
   * Only use cache for callee-saved registers (r4-r11) since scratch registers
   * like ip (r12) can be overwritten at any time without invalidating the cache. */
  TCCIRState *ir = tcc_state->ir;
  int cached_reg = -1;
  int is_callee_saved = (dest_reg >= R4 && dest_reg <= R11);

  if (ir && is_callee_saved && tcc_ir_opt_fp_cache_lookup(ir, frame_offset, &cached_reg))
  {
    /* Cache hit! Verify the cached register is also callee-saved */
    if (cached_reg >= R4 && cached_reg <= R11)
    {
      if (cached_reg != dest_reg)
      {
        ot_check_mov_reg(dest_reg, cached_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                         ENFORCE_ENCODING_NONE, false);
      }
      return;
    }
    /* Cached in scratch register - don't use it */
  }

  const int neg = (frame_offset < 0);
  int abs_off = neg ? -frame_offset : frame_offset;
  thumb_opcode op = neg ? th_sub_imm(dest_reg, base_reg, abs_off, flags_safe(), ENFORCE_ENCODING_NONE)
                        : th_add_imm(dest_reg, base_reg, abs_off, flags_safe(), ENFORCE_ENCODING_NONE);

  if (op.size != 0)
  {
    ot_check(op);
    /* Record in cache for future reuse - only for callee-saved registers
     * which won't be clobbered unexpectedly */
    if (ir && is_callee_saved)
      tcc_ir_opt_fp_cache_record(ir, frame_offset, dest_reg);
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

  load_full_const(offset_reg, PREG_NONE, LFC_SPLIT(frame_offset));
  ot_check(th_add_reg(dest_reg, base_reg, offset_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                      ENFORCE_ENCODING_NONE));

  if (dest_reg == base_reg)
  {
    restore_scratch_reg(&offset_alloc);
  }

  /* Record complex computation in cache - only for callee-saved registers */
  if (ir && is_callee_saved)
    tcc_ir_opt_fp_cache_record(ir, frame_offset, dest_reg);
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
      _lfc_sym = validated_sym;
      load_full_const(dest_reg, dest_reg_high, LFC_SPLIT(value));
      return;
    }
    /* Invalid or missing sym - fall through to treat as plain constant */
  }

  if (!sym && !is_64bit && dest_reg >= 0 && dest_reg < 16 &&
      imm_cache[dest_reg].valid && imm_cache[dest_reg].sym == NULL &&
      imm_cache[dest_reg].value == value)
    return;

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
    load_full_const(dest_reg, dest_reg_high, LFC_SPLIT(value));
    return;
  }

  /* 32-bit constant */
  if (!ot(th_generic_mov_imm(dest_reg, (uint32_t)value)))
    load_full_const(dest_reg, PREG_NONE, LFC_SPLIT(value));

  if (!sym && !is_64bit && dest_reg >= 0 && dest_reg < 16)
  {
    imm_cache[dest_reg].value = value;
    imm_cache[dest_reg].sym = NULL;
    imm_cache[dest_reg].valid = 1;
  }
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
static void load_from_base(int r, int r1, int irop_btype, int is_unsigned, int fc, int sign, uint32_t base)
{
  int success = 0;
  const int is_64bit =
      (irop_btype == IROP_BTYPE_INT64 || irop_btype == IROP_BTYPE_FLOAT64 || (r1 >= 0 && r1 != PREG_REG_NONE));

  TRACE("load_from_base: r=%d, r1=%d, irop_btype=%d, is_unsigned=%d, fc=%d, sign=%d, base=%d", r, r1, irop_btype,
        is_unsigned, fc, sign, base);

  if (is_64bit)
  {
    /* 64-bit value (double float or long long) - load to register pair */
    int ir_high = r1;
    ScratchRegAlloc ir_high_alloc = {0};
    if (ir_high < 0 || ir_high == PREG_REG_NONE)
    {
      /* No explicit high register — always use scratch to avoid clobbering
       * r+1 which may be allocated to another live variable.  The old r+1
       * fallback was only safe when mat.c pre-materialized into scratch
       * registers (ip:lr pair) before the handler. */
      ir_high_alloc = get_scratch_reg_with_save((1u << r) | (1u << base));
      ir_high = ir_high_alloc.reg;
    }

    /* If base overlaps with destination, preserve it */
    ScratchRegAlloc base_alloc = {0};
    uint32_t base_reg = base;
    if (base_reg == (uint32_t)r || base_reg == (uint32_t)ir_high)
    {
      uint32_t exclude = (1u << r) | (1u << ir_high);
      base_alloc = get_scratch_reg_with_save(exclude);
      base_reg = (uint32_t)base_alloc.reg;
      ot_check_mov_reg((int)base_reg, (int)base, flags_safe(), THUMB_SHIFT_DEFAULT,
                       ENFORCE_ENCODING_NONE, false);
    }

    /* Try LDRD Rt, Rt2, [Rn, #±imm] when both halves share one base.
     * T1 encoding requires: Rt != Rt2, Rt/Rt2 not SP/PC, offset 4-byte
     * aligned and |offset| <= 1020.  LDRD also requires the target address
     * to be 4-byte aligned on ARMv7-M/v8-M (faults otherwise, regardless of
     * UNALIGN_TRP).  Restrict to SP/FP-relative bases where TCC's stack
     * allocator guarantees 4-byte alignment of 64-bit slots; arbitrary
     * pointers (e.g. into a packed struct) may be unaligned. */
    const int base_is_stack = (base_reg == (uint32_t)R_SP || base_reg == (uint32_t)R_FP);
    if (base_is_stack && (fc & 3) == 0 && fc <= 1020 && r >= 0 && r <= R_LR && r != R_SP && ir_high >= 0 &&
        ir_high <= R_LR && ir_high != R_SP && r != ir_high)
    {
      uint32_t puw = sign ? 4 : 6;
      ot_check(th_ldrd_imm((uint32_t)r, (uint32_t)ir_high, base_reg, fc, puw));
      if (base_alloc.saved)
        restore_scratch_reg(&base_alloc);
      if (ir_high_alloc.saved)
        restore_scratch_reg(&ir_high_alloc);
      return;
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
    if (ir_high_alloc.saved)
      restore_scratch_reg(&ir_high_alloc);
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

ST_FUNC void gen_increment_tcov(SValue *sv)
{
  TRACE("'gen_increment_tcov'");
}

int th_has_immediate_value(int r)
{
  return (r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST;
}

typedef thumb_opcode (*thumb_reg_handler_t)(uint32_t rd, uint32_t rn, uint32_t rm,
                                            thumb_flags_behaviour flags_behaviour, thumb_shift shift_type,
                                            thumb_enforce_encoding enforce_encoding);
typedef struct ThumbDataProcessingHandler
{
  thumb_imm_handler_t imm_handler;
  thumb_reg_handler_t reg_handler;
} ThumbDataProcessingHandler;

/* Dispatch a reg_handler call through a direct call instead of an indirect
 * (function pointer) call.  This works around a code-generation bug where
 * struct-by-value arguments (thumb_shift) get corrupted when passed through
 * indirect calls that also use sret return (thumb_opcode is 8 bytes).
 * By comparing the function pointer and branching to a direct call, the
 * cross-compiler generates correct struct passing code. */
static thumb_opcode thumb_call_reg_handler(thumb_reg_handler_t fn, uint32_t rd, uint32_t rn, uint32_t rm,
                                           thumb_flags_behaviour flags, thumb_shift shift,
                                           thumb_enforce_encoding encoding)
{
  if (fn == th_add_reg)
    return th_add_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_sub_reg)
    return th_sub_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_adc_reg)
    return th_adc_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_sbc_reg)
    return th_sbc_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_cmp_reg)
    return th_cmp_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_lsl_reg)
    return th_lsl_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_lsr_reg)
    return th_lsr_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_asr_reg)
    return th_asr_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_orr_reg)
    return th_orr_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_and_reg)
    return th_and_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_eor_reg)
    return th_eor_reg(rd, rn, rm, flags, shift, encoding);
  if (fn == th_bic_reg)
    return th_bic_reg(rd, rn, rm, flags, shift, encoding);
  /* Unreachable for known handlers — fallback to direct call. */
  return fn(rd, rn, rm, flags, shift, encoding);
}

static void thumb_require_materialized_reg(const char *ctx, const char *operand, int reg)
{
  const bool reg_is_hw = (reg >= 0) && (reg <= 15);
  if (reg == PREG_REG_NONE || !reg_is_hw)
  {
    tcc_error("compiler_error: %s expects %s in a physical register (pr=%d)", ctx, operand, reg);
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

static void thumb_emit_op_imm_fallback(int rd, int rn, uint32_t imm, thumb_flags_behaviour flags,
                                       ThumbDataProcessingHandler handler)
{
  thumb_opcode sub_low = thumb_call_imm_handler(handler.imm_handler, rd, rn, imm, flags, ENFORCE_ENCODING_NONE);
  if (sub_low.size == 0)
  {
    uint32_t exclude = 0;
    if (rd >= 0 && rd <= 15)
      exclude |= (1u << rd);
    if (rn >= 0 && rn <= 15)
      exclude |= (1u << rn);
    ScratchRegAlloc scratch = get_scratch_reg_with_save(exclude);
    tcc_machine_load_constant(scratch.reg, PREG_NONE, (int32_t)imm, 0, NULL);
    ot_check(thumb_call_reg_handler(handler.reg_handler, rd, rn, scratch.reg, flags, THUMB_SHIFT_DEFAULT,
                                    ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&scratch);
  }
  else
  {
    ot_check(sub_low);
  }
}

typedef thumb_opcode (*thumb_regonly3_handler_t)(uint32_t rd, uint32_t rn, uint32_t rm);

static thumb_opcode thumb_mul_regonly(uint32_t rd, uint32_t rn, uint32_t rm)
{
  return th_mul(rd, rn, rm, flags_safe(), ENFORCE_ENCODING_NONE);
}

static thumb_opcode thumb_sdiv_regonly(uint32_t rd, uint32_t rn, uint32_t rm)
{
  return th_sdiv((uint16_t)rd, (uint16_t)rn, (uint16_t)rm);
}

static thumb_opcode thumb_udiv_regonly(uint32_t rd, uint32_t rn, uint32_t rm)
{
  return th_udiv((uint16_t)rd, (uint16_t)rn, (uint16_t)rm);
}

typedef thumb_opcode (*thumb_longmul_handler_t)(uint32_t rdlo, uint32_t rdhi, uint32_t rn, uint32_t rm);

/* ============================================================
 * mach_resolve_deref_64
 * ============================================================
 * When a 64-bit source has needs_deref=true, the operand holds a POINTER
 * to a 64-bit value — not the value itself.  Splitting such an operand
 * via mach_make_lo_half / mach_make_hi_half is WRONG because
 * mach_make_hi_half would increment the register number (e.g. R0 → R1)
 * instead of the memory offset.
 *
 * This helper resolves the deref by loading both 32-bit halves from
 * [base+0] and [base+4] into scratch registers, returning a clean
 * MACH_OP_REG pair operand with needs_deref=false.  The caller can
 * then safely call mach_make_lo_half / mach_make_hi_half on the result.
 *
 * Returns *op unchanged if needs_deref is false.
 */
static MachineOperand mach_resolve_deref_64(MachineCodegenContext *mctx, const MachineOperand *op, uint32_t *excl)
{
  if (!op->needs_deref)
    return *op;

  /* PARAM_STACK with needs_deref (is_lval): the 64-bit value IS directly
   * at [fp+offset], NOT a pointer to follow.  Clear needs_deref and let
   * the normal mach_make_lo_half / mach_make_hi_half path handle it. */
  if (op->kind == MACH_OP_PARAM_STACK)
  {
    MachineOperand result = *op;
    result.needs_deref = false;
    return result;
  }

  /* Strip deref to get the raw address into a register. */
  MachineOperand addr = *op;
  addr.needs_deref = false;
  addr.is_64bit = false;
  addr.btype = IROP_BTYPE_INT32;
  int base_reg = mach_ensure_in_reg(mctx, &addr, *excl);
  if (thumb_is_hw_reg(base_reg))
    *excl |= (1u << (uint32_t)base_reg);

  /* Allocate two scratch registers for the loaded halves. */
  int lo_reg = mach_alloc_scratch(mctx, *excl);
  *excl |= (1u << (uint32_t)lo_reg);
  int hi_reg = mach_alloc_scratch(mctx, *excl);
  *excl |= (1u << (uint32_t)hi_reg);

  /* Load [base+0] → lo, [base+4] → hi (32-bit loads). */
  load_from_base(lo_reg, PREG_REG_NONE, IROP_BTYPE_INT32, 0, 0, 0, (uint32_t)base_reg);
  load_from_base(hi_reg, PREG_REG_NONE, IROP_BTYPE_INT32, 0, 4, 0, (uint32_t)base_reg);

  /* Build a clean register-pair operand. */
  MachineOperand result = {0};
  result.kind = MACH_OP_REG;
  result.is_64bit = true;
  result.needs_deref = false;
  result.btype = op->btype;
  result.u.reg.r0 = lo_reg;
  result.u.reg.r1 = hi_reg;
  return result;
}

/* ============================================================
 * mach_make_lo_half / mach_make_hi_half
 * ============================================================
 * Split a 64-bit MachineOperand into its 32-bit low and high halves.
 * The resulting operands have is_64bit=false and represent the individual
 * 32-bit words, suitable for mach_ensure_in_reg / mach_writeback_dest.
 *
 * Only call mach_make_hi_half on a 64-bit operand (is_64bit=true or
 * MACH_OP_SPILL); the result for 32-bit REG is the next register (r0+1).
 */
static MachineOperand mach_make_lo_half(const MachineOperand *op)
{
  MachineOperand lo = *op;
  lo.is_64bit = false;
  if (lo.kind == MACH_OP_REG)
    lo.u.reg.r1 = -1;
  /* SPILL: keep the same offset — low word is at the base offset.   */
  /* IMM:   u.imm.val bits [31:0] are the low word (callers truncate). */
  /* CHAIN_REL: keep offset/chain_index — low word is at base offset. */
  return lo;
}

static MachineOperand mach_make_hi_half(const MachineOperand *op)
{
  MachineOperand hi = *op;
  hi.is_64bit = false;
  switch (hi.kind)
  {
  case MACH_OP_REG:
    /* r1 holds the high register for 64-bit pairs.  If r1 is not a valid
     * hardware register the allocator failed to produce a proper pair —
     * error out instead of silently using r0+1 which can clobber reserved
     * registers (e.g. R9 = GOT base). */
    if (!thumb_is_hw_reg(op->u.reg.r1))
      tcc_error("mach_make_hi_half: 64-bit REG operand has invalid r1=%d (r0=%d) — "
                "register allocator must produce a valid pair",
                op->u.reg.r1, op->u.reg.r0);
    hi.u.reg.r0 = op->u.reg.r1;
    hi.u.reg.r1 = -1;
    break;
  case MACH_OP_SPILL:
    hi.u.spill.offset += 4;
    break;
  case MACH_OP_IMM:
    hi.u.imm.val = (int64_t)(int32_t)(uint32_t)((uint64_t)op->u.imm.val >> 32);
    break;
  case MACH_OP_PARAM_STACK:
    hi.u.param.offset += 4;
    break;
  case MACH_OP_CHAIN_REL:
    hi.u.chain.offset += 4; /* high word is 4 bytes above low word */
    break;
  case MACH_OP_SYMBOL:
    hi.u.sym.addend += 4; /* high word at symbol + addend + 4 */
    break;
  case MACH_OP_FRAME_ADDR:
    hi.u.frame.offset += 4; /* high word at FP + offset + 4 */
    break;
  default:
    break;
  }
  return hi;
}

/* ============================================================
 * thumb_emit_data_processing_mop64
 * ============================================================
 * 64-bit ADD / SUB / AND / OR / XOR via MachineOperand register pairs.
 * Handles REG (r0:r1), SPILL (offset, offset+4) and IMM operands.
 *
 * uses_carry=true  → low word uses FLAGS_BEHAVIOUR_SET, high word uses the
 *                    carry handler (ADDS + ADC for ADD, SUBS + SBC for SUB).
 * uses_carry=false → both halves use the same handler independently (AND/OR/XOR).
 *
 * If src1 is not 64-bit (e.g. int promoted to long long), its high half is
 * zero-extended.  Similarly for src2.
 */
static void thumb_emit_data_processing_mop64(const MachineOperand *src1, const MachineOperand *src2,
                                             const MachineOperand *dest, TccIrOp op, ThumbDataProcessingHandler regular,
                                             ThumbDataProcessingHandler carry_h, bool uses_carry)
{
  (void)op;
  MachineCodegenContext mctx = {0};
  uint32_t excl = 0;

  /* 0. Determine destination register pair FIRST so that deref resolution
   *    never allocates scratch registers that overlap with the dest pair.
   *    Without this, mach_release_all would restore saved scratch regs
   *    and clobber the result sitting in rd_lo / rd_hi. */
  int rd_lo, rd_hi;
  bool store_lo = false, store_hi = false;
  if (dest->kind == MACH_OP_REG && !dest->needs_deref && dest->u.reg.r0 != (int)PREG_REG_NONE && dest->u.reg.r1 >= 0)
  {
    rd_lo = dest->u.reg.r0;
    rd_hi = dest->u.reg.r1;
    excl |= (1u << (uint32_t)rd_lo) | (1u << (uint32_t)rd_hi);
  }
  else
  {
    rd_lo = mach_alloc_scratch(&mctx, excl);
    excl |= (1u << (uint32_t)rd_lo);
    rd_hi = mach_alloc_scratch(&mctx, excl);
    excl |= (1u << (uint32_t)rd_hi);
    store_lo = store_hi = (dest->kind != MACH_OP_NONE);
  }

  /* 0b. Pre-exclude register operands so that deref resolution of one
   *     source never steals the physical registers of another source.
   *     This must include needs_deref registers: they hold live pointers
   *     that will be consumed during their own deref resolution and must
   *     not be repurposed as scratch during the other source's deref. */
  if (src1->kind == MACH_OP_REG)
  {
    if (src1->u.reg.r0 != (int)PREG_REG_NONE)
      excl |= (1u << (uint32_t)src1->u.reg.r0);
    if (!src1->needs_deref && src1->is_64bit && src1->u.reg.r1 >= 0)
      excl |= (1u << (uint32_t)src1->u.reg.r1);
  }
  if (src2->kind == MACH_OP_REG)
  {
    if (src2->u.reg.r0 != (int)PREG_REG_NONE)
      excl |= (1u << (uint32_t)src2->u.reg.r0);
    if (!src2->needs_deref && src2->is_64bit && src2->u.reg.r1 >= 0)
      excl |= (1u << (uint32_t)src2->u.reg.r1);
  }

  /* 1. Resolve deref'd source pointers before splitting into halves. */
  MachineOperand r_src1 = mach_resolve_deref_64(&mctx, src1, &excl);
  src1 = &r_src1;
  MachineOperand r_src2 = mach_resolve_deref_64(&mctx, src2, &excl);
  src2 = &r_src2;

  /* 2. Load src1 low and high halves into registers. */
  MachineOperand s1_lo = mach_make_lo_half(src1);
  int rn_lo = mach_ensure_in_reg(&mctx, &s1_lo, excl);
  if (thumb_is_hw_reg(rn_lo))
    excl |= (1u << (uint32_t)rn_lo);
  int rn_hi;
  if (src1->is_64bit)
  {
    MachineOperand s1_hi = mach_make_hi_half(src1);
    rn_hi = mach_ensure_in_reg(&mctx, &s1_hi, excl);
  }
  else
  {
    rn_hi = mach_alloc_scratch(&mctx, excl);
    ot_check(th_mov_imm((uint32_t)rn_hi, 0, flags_safe(), ENFORCE_ENCODING_NONE));
  }
  if (thumb_is_hw_reg(rn_hi))
    excl |= (1u << (uint32_t)rn_hi);

  /* 3. Load src2 and emit the 64-bit operation. */
  const thumb_flags_behaviour lo_flags = uses_carry ? FLAGS_BEHAVIOUR_SET : flags_safe();
  /* For CMP, the high-word SBCS must set flags (the following SETIF reads them). */
  const thumb_flags_behaviour hi_flags = (op == TCCIR_OP_CMP) ? FLAGS_BEHAVIOUR_SET : flags_safe();
  if (src2->kind == MACH_OP_IMM)
  {
    const uint32_t imm_lo = (uint32_t)((uint64_t)src2->u.imm.val & 0xffffffffu);
    const uint32_t imm_hi = (uint32_t)((uint64_t)src2->u.imm.val >> 32);
    /* Per-half peephole: when the immediate half makes the op a constant
     * answer (OR/XOR with 0 → copy src; AND with 0 → load 0; AND with -1 →
     * copy src), skip the data-processing op.  Cuts dead `orr r, r, #0` and
     * `and r, r, #0` halves left behind by 64-bit ops on 32-bit values. */
    const bool is_or = (op == TCCIR_OP_OR);
    const bool is_xor = (op == TCCIR_OP_XOR);
    const bool is_and = (op == TCCIR_OP_AND);
    const bool can_simplify_lo = lo_flags == flags_safe();
    const bool can_simplify_hi = hi_flags == flags_safe();
    for (int half = 0; half < 2; half++)
    {
      const uint32_t imm = (half == 0) ? imm_lo : imm_hi;
      const int rd = (half == 0) ? rd_lo : rd_hi;
      const int rn = (half == 0) ? rn_lo : rn_hi;
      const thumb_flags_behaviour fb = (half == 0) ? lo_flags : hi_flags;
      const bool can_simplify = (half == 0) ? can_simplify_lo : can_simplify_hi;
      const ThumbDataProcessingHandler *h = (half == 0) ? &regular : &carry_h;

      if (can_simplify && (is_or || is_xor) && imm == 0)
      {
        if (rd != rn)
          ot_check_mov_reg((uint32_t)rd, (uint32_t)rn, flags_safe(),
                           THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
      }
      else if (can_simplify && is_and && imm == 0)
      {
        ot_check(th_mov_imm((uint32_t)rd, 0, flags_safe(), ENFORCE_ENCODING_NONE));
      }
      else if (can_simplify && is_and && imm == 0xFFFFFFFFu)
      {
        if (rd != rn)
          ot_check_mov_reg((uint32_t)rd, (uint32_t)rn, flags_safe(),
                           THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
      }
      else
      {
        thumb_emit_op_imm_fallback(rd, rn, imm, fb, *h);
      }
    }
  }
  else
  {
    MachineOperand s2_lo = mach_make_lo_half(src2);
    int rm_lo = mach_ensure_in_reg(&mctx, &s2_lo, excl);
    if (thumb_is_hw_reg(rm_lo))
      excl |= (1u << (uint32_t)rm_lo);
    int rm_hi;
    if (src2->is_64bit)
    {
      MachineOperand s2_hi = mach_make_hi_half(src2);
      rm_hi = mach_ensure_in_reg(&mctx, &s2_hi, excl);
    }
    else
    {
      rm_hi = mach_alloc_scratch(&mctx, excl);
      ot_check(th_mov_imm((uint32_t)rm_hi, 0, flags_safe(), ENFORCE_ENCODING_NONE));
    }
    {
      ot_check(thumb_call_reg_handler(regular.reg_handler, (uint32_t)rd_lo, (uint32_t)rn_lo, (uint32_t)rm_lo, lo_flags,
                                      THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      ot_check(thumb_call_reg_handler(carry_h.reg_handler, (uint32_t)rd_hi, (uint32_t)rn_hi, (uint32_t)rm_hi,
                                      hi_flags, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
  }

  /* 4. Write results back to spill/param slots if dest was not pre-allocated. */
  if (store_lo)
  {
    MachineOperand dst_lo = mach_make_lo_half(dest);
    dst_lo.btype = IROP_BTYPE_INT32;
    mach_writeback_dest(&dst_lo, rd_lo);
  }
  if (store_hi)
  {
    MachineOperand dst_hi = mach_make_hi_half(dest);
    dst_hi.btype = IROP_BTYPE_INT32;
    mach_writeback_dest(&dst_hi, rd_hi);
  }
  mach_release_all(&mctx);
}

/* ============================================================
 * thumb_emit_shift64_mop
 * ============================================================
 * 64-bit SHL / SHR / SAR via MachineOperand register pairs.
 * Shift amount (src2) must be a 32-bit immediate (MACH_OP_IMM).
 * Logic mirrors thumb_emit_shift64_imm but operates on register numbers
 * extracted from MachineOperand rather than IROperand fields.
 */
static void thumb_emit_shift64_mop(const MachineOperand *src1, const MachineOperand *src2, const MachineOperand *dest,
                                   TccIrOp op, bool skip_lo, bool skip_hi)
{
  if (src2->kind != MACH_OP_IMM)
  {
    tcc_error("compiler_error: thumb_emit_shift64_mop: non-immediate shift count");
    return;
  }
  const uint32_t sh = (uint32_t)(uint64_t)src2->u.imm.val;
  const bool is_left = (op == TCCIR_OP_SHL);
  const bool arith_right = (op == TCCIR_OP_SAR);

  thumb_imm_handler_t dst_lo_shift, dst_hi_shift, cross_shift;
  if (is_left)
  {
    dst_lo_shift = th_lsl_imm;
    dst_hi_shift = th_lsl_imm;
    cross_shift = th_lsr_imm;
  }
  else if (arith_right)
  {
    dst_lo_shift = th_lsr_imm;
    dst_hi_shift = th_asr_imm;
    cross_shift = th_lsl_imm;
  }
  else
  {
    dst_lo_shift = th_lsr_imm;
    dst_hi_shift = th_lsr_imm;
    cross_shift = th_lsl_imm;
  }

  MachineCodegenContext mctx = {0};
  uint32_t excl = 0;

  /* Determine destination register pair FIRST so that deref resolution
   * never allocates scratch registers that overlap with the dest pair. */
  int dst_lo, dst_hi;
  bool store_lo = false, store_hi = false;
  if (dest->kind == MACH_OP_REG && !dest->needs_deref && dest->u.reg.r0 != (int)PREG_REG_NONE && dest->u.reg.r1 >= 0)
  {
    dst_lo = dest->u.reg.r0;
    dst_hi = dest->u.reg.r1;
    excl |= (1u << (uint32_t)dst_lo) | (1u << (uint32_t)dst_hi);
  }
  else
  {
    dst_lo = mach_alloc_scratch(&mctx, excl);
    excl |= (1u << (uint32_t)dst_lo);
    dst_hi = mach_alloc_scratch(&mctx, excl);
    excl |= (1u << (uint32_t)dst_hi);
    store_lo = store_hi = (dest->kind != MACH_OP_NONE);
  }

  /* Pre-exclude register operands so that deref resolution does not
   * steal the physical registers already holding src1 values.
   * Include needs_deref registers: they hold live pointers needed
   * during their own deref resolution. */
  if (src1->kind == MACH_OP_REG)
  {
    if (src1->u.reg.r0 != (int)PREG_REG_NONE)
      excl |= (1u << (uint32_t)src1->u.reg.r0);
    if (!src1->needs_deref && src1->is_64bit && src1->u.reg.r1 >= 0)
      excl |= (1u << (uint32_t)src1->u.reg.r1);
  }

  /* Resolve deref'd source pointer before splitting into halves. */
  MachineOperand r_src1 = mach_resolve_deref_64(&mctx, src1, &excl);
  src1 = &r_src1;

  /* Load src1 low half. */
  MachineOperand s1_lo = mach_make_lo_half(src1);
  int src_lo = mach_ensure_in_reg(&mctx, &s1_lo, excl);
  if (thumb_is_hw_reg(src_lo))
    excl |= (1u << (uint32_t)src_lo);

  /* Skip src1 high-half materialization when the shift will not read it.
   * SHL with sh >= 32 only uses src_lo (everything shifts up out of view).
   * SHR/SAR with sh >= 64 produces a 0/sign-fill that the emit tail
   * generates directly without referencing src_hi. */
  int hi_needed = 1;
  if (is_left && sh >= 32)
    hi_needed = 0;
  else if (!is_left && sh >= 64)
    hi_needed = 0;

  /* Load src1 high half or compute by extension. */
  int src_hi = (int)PREG_REG_NONE;
  if (src1->is_64bit)
  {
    MachineOperand s1_hi = mach_make_hi_half(src1);
    src_hi = mach_ensure_in_reg(&mctx, &s1_hi, excl);
    if (thumb_is_hw_reg(src_hi))
      excl |= (1u << (uint32_t)src_hi);
  }
  else if (hi_needed)
  {
    src_hi = mach_alloc_scratch(&mctx, excl);
    excl |= (1u << (uint32_t)src_hi);
    if (arith_right)
      ot_check(
          th_asr_imm((uint32_t)src_hi, (uint32_t)src_lo, 31, flags_safe(), ENFORCE_ENCODING_NONE));
    else
      ot_check(th_mov_imm((uint32_t)src_hi, 0, flags_safe(), ENFORCE_ENCODING_NONE));
  }

  /* Emit the shift — logic identical to thumb_emit_shift64_imm core. */
  if (sh == 0)
  {
    ot_check_mov_reg((uint32_t)dst_lo, (uint32_t)src_lo, flags_safe(), THUMB_SHIFT_DEFAULT,
                     ENFORCE_ENCODING_NONE, false);
    ot_check_mov_reg((uint32_t)dst_hi, (uint32_t)src_hi, flags_safe(), THUMB_SHIFT_DEFAULT,
                     ENFORCE_ENCODING_NONE, false);
  }
  else if (sh < 32)
  {
    const int regs[] = {dst_lo, dst_hi, src_lo, src_hi};
    ScratchRegAlloc tmp = get_scratch_reg_with_save(thumb_exclude_mask_for_regs(4, regs) | excl);
    if (is_left)
    {
      /* Compute the cross-shift into tmp BEFORE any destination is written,
       * because dst_lo/dst_hi may alias src_lo/src_hi. */
      ot_check(thumb_call_imm_handler(cross_shift, (uint32_t)tmp.reg, (uint32_t)src_lo, 32 - sh, flags_safe(),
                           ENFORCE_ENCODING_NONE));
      if (dst_hi == src_lo)
      {
        /* dst_hi aliases src_lo — compute dst_lo first (needs src_lo). */
        if (!skip_lo)
          ot_check(
              thumb_call_imm_handler(dst_lo_shift, (uint32_t)dst_lo, (uint32_t)src_lo, sh, flags_safe(), ENFORCE_ENCODING_NONE));
        ot_check(
            thumb_call_imm_handler(dst_hi_shift, (uint32_t)dst_hi, (uint32_t)src_hi, sh, flags_safe(), ENFORCE_ENCODING_NONE));
      }
      else
      {
        /* Default order: dst_hi first to avoid clobbering src_hi via dst_lo. */
        ot_check(
            thumb_call_imm_handler(dst_hi_shift, (uint32_t)dst_hi, (uint32_t)src_hi, sh, flags_safe(), ENFORCE_ENCODING_NONE));
        if (!skip_lo)
          ot_check(
              thumb_call_imm_handler(dst_lo_shift, (uint32_t)dst_lo, (uint32_t)src_lo, sh, flags_safe(), ENFORCE_ENCODING_NONE));
      }
      ot_check(th_orr_reg((uint32_t)dst_hi, (uint32_t)dst_hi, (uint32_t)tmp.reg, flags_safe(),
                          THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    else
    {
      /* Compute the cross-shift into tmp BEFORE any destination is written,
       * because dst_lo/dst_hi may alias src_lo/src_hi. */
      ot_check(thumb_call_imm_handler(cross_shift, (uint32_t)tmp.reg, (uint32_t)src_hi, 32 - sh, flags_safe(),
                           ENFORCE_ENCODING_NONE));
      if (dst_lo == src_hi)
      {
        /* dst_lo aliases src_hi — compute dst_hi first (needs src_hi). */
        ot_check(
            thumb_call_imm_handler(dst_hi_shift, (uint32_t)dst_hi, (uint32_t)src_hi, sh, flags_safe(), ENFORCE_ENCODING_NONE));
        ot_check(
            th_lsr_imm((uint32_t)dst_lo, (uint32_t)src_lo, sh, flags_safe(), ENFORCE_ENCODING_NONE));
      }
      else
      {
        /* Default order: dst_lo first to avoid clobbering src_lo via dst_hi. */
        ot_check(
            th_lsr_imm((uint32_t)dst_lo, (uint32_t)src_lo, sh, flags_safe(), ENFORCE_ENCODING_NONE));
        ot_check(
            thumb_call_imm_handler(dst_hi_shift, (uint32_t)dst_hi, (uint32_t)src_hi, sh, flags_safe(), ENFORCE_ENCODING_NONE));
      }
      ot_check(th_orr_reg((uint32_t)dst_lo, (uint32_t)dst_lo, (uint32_t)tmp.reg, flags_safe(),
                          THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    restore_scratch_reg(&tmp);
  }
  else if (sh == 32)
  {
    if (is_left)
    {
      /* Emit MOV dst_hi first: dst_lo may alias src_lo. */
      ot_check_mov_reg((uint32_t)dst_hi, (uint32_t)src_lo, flags_safe(), THUMB_SHIFT_DEFAULT,
                       ENFORCE_ENCODING_NONE, false);
      if (!skip_lo)
        ot_check(th_mov_imm((uint32_t)dst_lo, 0, flags_safe(), ENFORCE_ENCODING_NONE));
    }
    else
    {
      /* Emit MOV dst_lo first: dst_hi may alias src_hi. */
      ot_check_mov_reg((uint32_t)dst_lo, (uint32_t)src_hi, flags_safe(), THUMB_SHIFT_DEFAULT,
                       ENFORCE_ENCODING_NONE, false);
      if (!skip_hi)
      {
        if (arith_right)
          ot_check(
              th_asr_imm((uint32_t)dst_hi, (uint32_t)src_hi, 31, flags_safe(), ENFORCE_ENCODING_NONE));
        else
          ot_check(th_mov_imm((uint32_t)dst_hi, 0, flags_safe(), ENFORCE_ENCODING_NONE));
      }
    }
  }
  else if (sh < 64)
  {
    if (is_left)
    {
      /* Emit shift into dst_hi first: dst_lo may alias src_lo. */
      ot_check(thumb_call_imm_handler(dst_hi_shift, (uint32_t)dst_hi, (uint32_t)src_lo, sh - 32, flags_safe(),
                            ENFORCE_ENCODING_NONE));
      if (!skip_lo)
        ot_check(th_mov_imm((uint32_t)dst_lo, 0, flags_safe(), ENFORCE_ENCODING_NONE));
    }
    else
    {
      if (arith_right && dst_lo == src_hi)
      {
        /* dst_lo aliases src_hi — compute dst_hi (sign extension) first
         * while src_hi is still intact, then shift into dst_lo. */
        if (!skip_hi)
          ot_check(
              th_asr_imm((uint32_t)dst_hi, (uint32_t)src_hi, 31, flags_safe(), ENFORCE_ENCODING_NONE));
        ot_check(thumb_call_imm_handler(dst_hi_shift, (uint32_t)dst_lo, (uint32_t)src_hi, sh - 32, flags_safe(),
                              ENFORCE_ENCODING_NONE));
      }
      else
      {
        ot_check(thumb_call_imm_handler(dst_hi_shift, (uint32_t)dst_lo, (uint32_t)src_hi, sh - 32, flags_safe(),
                              ENFORCE_ENCODING_NONE));
        if (!skip_hi)
        {
          if (arith_right)
            ot_check(
                th_asr_imm((uint32_t)dst_hi, (uint32_t)src_hi, 31, flags_safe(), ENFORCE_ENCODING_NONE));
          else
            ot_check(th_mov_imm((uint32_t)dst_hi, 0, flags_safe(), ENFORCE_ENCODING_NONE));
        }
      }
    }
  }
  else /* sh >= 64 */
  {
    if (is_left)
    {
      if (!skip_lo)
        ot_check(th_mov_imm((uint32_t)dst_lo, 0, flags_safe(), ENFORCE_ENCODING_NONE));
      if (!skip_hi)
        ot_check(th_mov_imm((uint32_t)dst_hi, 0, flags_safe(), ENFORCE_ENCODING_NONE));
    }
    else if (arith_right)
    {
      /* Both halves are the sign of src_hi; dst_lo copies dst_hi, so leave
       * this degenerate path intact rather than risk the inter-half dep. */
      ot_check(
          th_asr_imm((uint32_t)dst_hi, (uint32_t)src_hi, 31, flags_safe(), ENFORCE_ENCODING_NONE));
      ot_check_mov_reg((uint32_t)dst_lo, (uint32_t)dst_hi, flags_safe(), THUMB_SHIFT_DEFAULT,
                       ENFORCE_ENCODING_NONE, false);
    }
    else
    {
      if (!skip_lo)
        ot_check(th_mov_imm((uint32_t)dst_lo, 0, flags_safe(), ENFORCE_ENCODING_NONE));
      if (!skip_hi)
        ot_check(th_mov_imm((uint32_t)dst_hi, 0, flags_safe(), ENFORCE_ENCODING_NONE));
    }
  }

  /* Write back.  A dead half was never materialized, so skip its store. */
  if (store_lo && !skip_lo)
  {
    MachineOperand dst_lo_op = mach_make_lo_half(dest);
    dst_lo_op.btype = IROP_BTYPE_INT32;
    mach_writeback_dest(&dst_lo_op, dst_lo);
  }
  if (store_hi && !skip_hi)
  {
    MachineOperand dst_hi_op = mach_make_hi_half(dest);
    dst_hi_op.btype = IROP_BTYPE_INT32;
    mach_writeback_dest(&dst_hi_op, dst_hi);
  }
  mach_release_all(&mctx);
}

/* ============================================================
 * MachineOperand-based data processing (_mop path)
 * ============================================================
 * thumb_emit_data_processing_mop32: simplified version of
 * thumb_emit_data_processing_op32 using MachineOperand instead of IROperand.
 * Handles 32-bit non-complex arithmetic/logic ops via the mach_* helpers,
 * eliminating the two-layer materialization present in the old path.
 */
static void thumb_emit_data_processing_mop32(const MachineOperand *src1, const MachineOperand *src2,
                                             const MachineOperand *dest, TccIrOp op, ThumbDataProcessingHandler handler,
                                             thumb_flags_behaviour flags, uint32_t barrel_shift)
{
  const bool dest_sets_flags = (op == TCCIR_OP_CMP);
  MachineCodegenContext mctx = {0};

  /* RSB fast path: SUB with immediate src1 → RSB Rd, src2, #imm.
   * Avoids materializing the immediate into a register.
   * Only attempt when the immediate is encodable as a Thumb-2 modified
   * constant (th_pack_const returns non-zero, or imm==0). */
  if (op == TCCIR_OP_SUB && !dest_sets_flags && barrel_shift == 0 &&
      src1->kind == MACH_OP_IMM && !src1->needs_deref && !src1->is_64bit)
  {
    uint32_t imm = (uint32_t)src1->u.imm.val;
    if (imm == 0 || th_pack_const(imm) != 0)
    {
      int dest_reg = mach_get_dest_reg(&mctx, dest, 0);
      uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;
      int src2_reg = mach_ensure_in_reg(&mctx, src2, excl);
      ot_check(th_rsb_imm((uint32_t)dest_reg, (uint32_t)src2_reg, imm, flags, ENFORCE_ENCODING_NONE));
      if (dest->kind != MACH_OP_NONE)
      {
        const bool needs_wb = dest->kind == MACH_OP_SPILL || dest->kind == MACH_OP_PARAM_STACK ||
                              (dest->kind == MACH_OP_REG && (dest->needs_deref || dest->u.reg.r0 == (int)PREG_REG_NONE));
        if (needs_wb)
          mach_writeback_dest(dest, dest_reg);
      }
      mach_release_all(&mctx);
      return;
    }
  }

  /* UXTB/UXTH fast path: AND with #0xFF or #0xFFFF → UXTB/UXTH.
   * 16-bit encoding (2 bytes) vs 32-bit AND immediate (4 bytes). */
  if (op == TCCIR_OP_AND && !dest_sets_flags && barrel_shift == 0 &&
      src2->kind == MACH_OP_IMM && !src2->needs_deref && !src2->is_64bit &&
      flags != FLAGS_BEHAVIOUR_SET)
  {
    uint32_t mask = (uint32_t)src2->u.imm.val;
    if (mask == 0xFF || mask == 0xFFFF)
    {
      int dest_reg = mach_get_dest_reg(&mctx, dest, 0);
      uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;
      int src1_reg = mach_ensure_in_reg(&mctx, src1, excl);
      if (mask == 0xFF)
        ot_check(th_uxtb((uint32_t)dest_reg, (uint32_t)src1_reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      else
        ot_check(th_uxth((uint32_t)dest_reg, (uint32_t)src1_reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      if (dest->kind != MACH_OP_NONE)
      {
        const bool needs_wb = dest->kind == MACH_OP_SPILL || dest->kind == MACH_OP_PARAM_STACK ||
                              (dest->kind == MACH_OP_REG && (dest->needs_deref || dest->u.reg.r0 == (int)PREG_REG_NONE));
        if (needs_wb)
          mach_writeback_dest(dest, dest_reg);
      }
      mach_release_all(&mctx);
      return;
    }
  }

  /* UBFX fast path: AND with a low-contiguous mask #((1<<W)-1) that is NOT
   * encodable as a Thumb-2 modified immediate → UBFX Rd, Rn, #0, #W.  Without
   * this the mask needs a separate movw to materialize (e.g. 0x7ff for an
   * 11-bit bitfield), so AND becomes two instructions; UBFX #0,#W is one and
   * semantically identical for the unsigned low-bits mask.  W==8/16 are handled
   * by the UXTB/UXTH path above, and any encodable mask stays a 1-instruction
   * AND (no win), so this only fires when it strictly removes the movw. */
  if (op == TCCIR_OP_AND && !dest_sets_flags && barrel_shift == 0 &&
      src2->kind == MACH_OP_IMM && !src2->needs_deref && !src2->is_64bit &&
      flags != FLAGS_BEHAVIOUR_SET)
  {
    uint32_t mask = (uint32_t)src2->u.imm.val;
    if (mask != 0 && mask != 0xFFFFFFFFu && (mask & (mask + 1)) == 0 && th_pack_const(mask) == 0)
    {
      int width = 0;
      while ((mask >> width) & 1u)
        width++;
      int dest_reg = mach_get_dest_reg(&mctx, dest, 0);
      uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;
      int src1_reg = mach_ensure_in_reg(&mctx, src1, excl);
      int widthm1 = width - 1;
      thumb_opcode ubfx_op;
      ubfx_op.size = 4;
      ubfx_op.opcode =
          0xF3C00000 | ((uint32_t)src1_reg << 16) | ((uint32_t)dest_reg << 8) | (uint32_t)widthm1;
      ot(ubfx_op);
      if (dest->kind != MACH_OP_NONE)
      {
        const bool needs_wb = dest->kind == MACH_OP_SPILL || dest->kind == MACH_OP_PARAM_STACK ||
                              (dest->kind == MACH_OP_REG && (dest->needs_deref || dest->u.reg.r0 == (int)PREG_REG_NONE));
        if (needs_wb)
          mach_writeback_dest(dest, dest_reg);
      }
      mach_release_all(&mctx);
      return;
    }
  }

  /* 1. Determine dest register (allocate scratch for spills/param/no-reg).
   * CMP and other flag-setting ops don't write a result register, so we
   * use R0 as a dummy (Rd field is architecturally ignored). */
  int dest_reg;
  if (dest_sets_flags)
    dest_reg = R0;
  else
    dest_reg = mach_get_dest_reg(&mctx, dest, 0);

  uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;

  /* Exclude src2's register from scratch allocation for src1.
   * Without this, materializing an immediate for src1 could pick src2's
   * register, clobbering it before src2 is read.  This applies whether
   * src2 is a plain register or a dereferenced one (the address register
   * must survive until the load). */
  if (src2->kind == MACH_OP_REG && thumb_is_hw_reg(src2->u.reg.r0))
    excl |= (1u << (uint32_t)src2->u.reg.r0);

  /* 2. Ensure src1 is in a register; add it to the exclusion mask. */
  int src1_reg = mach_ensure_in_reg(&mctx, src1, excl);
  if (thumb_is_hw_reg(src1_reg))
    excl |= (1u << (uint32_t)src1_reg);

  /* 3. Try immediate form for src2; fall back to register if needed. */
  bool imm_emitted = false;
  int src2_reg =
      mach_ensure_imm_or_reg(&mctx, src2, excl, handler.imm_handler, dest_reg, src1_reg, flags, &imm_emitted);
  if (!imm_emitted)
  {
    /* Decode barrel shift annotation (0=none, else type<<5|amount). */
    thumb_shift sh = THUMB_SHIFT_DEFAULT;
    if (barrel_shift != 0)
    {
      static const thumb_shift_type bs_map[] = {
        [1] = THUMB_SHIFT_LSL, [2] = THUMB_SHIFT_LSR,
        [3] = THUMB_SHIFT_ASR, [4] = THUMB_SHIFT_ROR,
      };
      uint32_t stype = (barrel_shift >> 5) & 7;
      uint32_t samt = barrel_shift & 31;
      sh.type = bs_map[stype];
      sh.value = samt;
      sh.mode = THUMB_SHIFT_IMMEDIATE;
    }
    thumb_enforce_encoding enc = (barrel_shift != 0) ? ENFORCE_ENCODING_32BIT : ENFORCE_ENCODING_NONE;
    ot_check(thumb_call_reg_handler(handler.reg_handler, (uint32_t)dest_reg, (uint32_t)src1_reg, (uint32_t)src2_reg,
                                    flags, sh, enc));
  }

  /* 4. Write result back to spill slot / stack param / pointer-dest. */
  if (!dest_sets_flags && dest && dest->kind != MACH_OP_NONE)
  {
    const bool needs_wb = dest->kind == MACH_OP_SPILL || dest->kind == MACH_OP_PARAM_STACK ||
                          (dest->kind == MACH_OP_REG && (dest->needs_deref || dest->u.reg.r0 == (int)PREG_REG_NONE));
    if (needs_wb)
      mach_writeback_dest(dest, dest_reg);
  }

  /* 5. Release all scratches in LIFO order. */
  mach_release_all(&mctx);
}

/* tcc_gen_machine_data_processing_mop: MachineOperand-based entry point for
 * arithmetic/logic operations.  Called from ir/codegen.c when dest does not
 * use a static chain register.
 * Dispatches to thumb_emit_data_processing_mop64 / thumb_emit_shift64_mop for
 * 64-bit pair destinations, or thumb_emit_data_processing_mop32 for 32-bit.
 */
static void data_processing_mop_impl(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op,
                                     thumb_flags_behaviour flags_override, uint32_t barrel_shift);

void tcc_gen_machine_data_processing_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op,
                                         uint32_t barrel_shift)
{
  data_processing_mop_impl(src1, src2, dest, op, flags_safe(), barrel_shift);
}

void tcc_gen_machine_data_processing_mop_flags(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op)
{
  data_processing_mop_impl(src1, src2, dest, op, FLAGS_BEHAVIOUR_SET, 0);
}

static void data_processing_mop_impl(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op,
                                     thumb_flags_behaviour flags_override, uint32_t barrel_shift)
{
  ThumbDataProcessingHandler handler;
  ThumbDataProcessingHandler carry_handler; /* used for hi word of 64-bit ops */
  bool uses_carry = false;
  /* CMP always sets flags — it has no non-flag-setting variant.
   * Ignore FLAGS_BEHAVIOUR_BLOCK for CMP; it must always use SET. */
  thumb_flags_behaviour flags = (op == TCCIR_OP_CMP) ? FLAGS_BEHAVIOUR_SET : flags_override;

  switch (op)
  {
  case TCCIR_OP_ADD:
    handler.imm_handler = th_add_imm;
    handler.reg_handler = th_add_reg;
    carry_handler.imm_handler = th_adc_imm;
    carry_handler.reg_handler = th_adc_reg;
    uses_carry = true;
    break;
  case TCCIR_OP_SUB:
    handler.imm_handler = th_sub_imm;
    handler.reg_handler = th_sub_reg;
    carry_handler.imm_handler = th_sbc_imm;
    carry_handler.reg_handler = th_sbc_reg;
    uses_carry = true;
    break;
  case TCCIR_OP_CMP:
    handler.imm_handler = th_cmp_imm_handler;
    handler.reg_handler = th_cmp_reg;
    carry_handler.imm_handler = th_sbc_imm;
    carry_handler.reg_handler = th_sbc_reg;
    uses_carry = true;
    break;
  case TCCIR_OP_SHL:
    handler.imm_handler = th_lsl_imm;
    handler.reg_handler = th_lsl_reg;
    carry_handler = handler;
    break;
  case TCCIR_OP_SHR:
    handler.imm_handler = th_lsr_imm;
    handler.reg_handler = th_lsr_reg;
    carry_handler = handler;
    break;
  case TCCIR_OP_SAR:
    handler.imm_handler = th_asr_imm;
    handler.reg_handler = th_asr_reg;
    carry_handler = handler;
    break;
  case TCCIR_OP_ROR:
    handler.imm_handler = th_ror_imm;
    handler.reg_handler = th_ror_reg;
    carry_handler = handler;
    break;
  case TCCIR_OP_OR:
    handler.imm_handler = th_orr_imm;
    handler.reg_handler = th_orr_reg;
    carry_handler = handler;
    break;
  case TCCIR_OP_AND:
    handler.imm_handler = th_and_imm;
    handler.reg_handler = th_and_reg;
    carry_handler = handler;
    break;
  case TCCIR_OP_XOR:
    handler.imm_handler = th_eor_imm;
    handler.reg_handler = th_eor_reg;
    carry_handler = handler;
    break;
  case TCCIR_OP_ADC_GEN:
    flags = FLAGS_BEHAVIOUR_SET;
    /* fall through */
  case TCCIR_OP_ADC_USE:
    handler.imm_handler = th_adc_imm;
    handler.reg_handler = th_adc_reg;
    carry_handler = handler;
    break;
  default:
    tcc_error("compiler_error: tcc_gen_machine_data_processing_mop: unhandled op %d", (int)op);
    return;
  }

  /* Dispatch 64-bit pair destinations to the mop64 path.
   * CMP has no dest (MACH_OP_NONE), so also check src1 for 64-bit. */
  if (dest.is_64bit || (op == TCCIR_OP_CMP && src1.is_64bit))
  {
    if (op == TCCIR_OP_SHL || op == TCCIR_OP_SHR || op == TCCIR_OP_SAR)
    {
      bool skip_lo = (barrel_shift >> 16) & 1;
      bool skip_hi = (barrel_shift >> 17) & 1;
      thumb_emit_shift64_mop(&src1, &src2, &dest, op, skip_lo, skip_hi);
    }
    else
      thumb_emit_data_processing_mop64(&src1, &src2, &dest, op, handler, carry_handler, uses_carry);
    return;
  }

  thumb_emit_data_processing_mop32(&src1, &src2, &dest, op, handler, flags, barrel_shift & 0xFFFFu);
}

/* tcc_gen_machine_ubfx_mop: emit UBFX Rd, Rn, #lsb, #width.
 * src2 encodes lsb (bits 0-4) and width (bits 5-9). */
void tcc_gen_machine_ubfx_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  MachineCodegenContext ctx = {0};
  int rd = mach_get_dest_reg(&ctx, &dest, 0);
  uint32_t excl = (1u << (uint32_t)rd);
  int rn = mach_ensure_in_reg(&ctx, &src1, excl);
  int param = (src2.kind == MACH_OP_IMM) ? (int)src2.u.imm.val : 0;
  int lsb = param & 0x1F;
  int width = (param >> 5) & 0x1F;
  if (width == 0)
    width = 8;
  int widthm1 = width - 1;
  int imm3 = (lsb >> 2) & 0x7;
  int imm2 = lsb & 0x3;
  /* Thumb-2 UBFX encoding: 11110 0 11 1100 Rn | 0 imm3 Rd imm2 0 widthm1 */
  thumb_opcode op;
  op.size = 4;
  op.opcode = 0xF3C00000 | ((uint32_t)rn << 16) | ((uint32_t)imm3 << 12) | ((uint32_t)rd << 8) | ((uint32_t)imm2 << 6) | (uint32_t)widthm1;
  ot(op);
  mach_writeback_dest(&dest, rd);
  mach_release_all(&ctx);
}

/* tcc_gen_machine_bfi_mop: emit BFI Rd, Rn, #lsb, #width.
 * src1 = host word (moved into Rd, the BFI base, if not already there),
 * src2 = value supplying the field bits (only its low `width` bits are used),
 * dest = result.  params packs lsb (bits 0-7) and width (bits 8-15). */
void tcc_gen_machine_bfi_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, uint32_t params)
{
  MachineCodegenContext ctx = {0};
  int rd = mach_get_dest_reg(&ctx, &dest, 0);
  int rn = mach_ensure_in_reg(&ctx, &src2, 0);                            /* value (Rn) */
  int rword = mach_ensure_in_reg(&ctx, &src1, (1u << (uint32_t)rn));      /* host word */
  /* Establish Rd = host word.  If the value happens to live in Rd (RA coalesced
   * the result onto src2), preserve it in a scratch before clobbering Rd. */
  if (rd != rword)
  {
    if (rd == rn)
    {
      int tmp = mach_alloc_scratch(&ctx, (1u << (uint32_t)rd) | (1u << (uint32_t)rword));
      ot_check_mov_reg((uint32_t)tmp, (uint32_t)rd, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
      rn = tmp;
    }
    ot_check_mov_reg((uint32_t)rd, (uint32_t)rword, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
  }
  int lsb = (int)(params & 0xFF);
  int width = (int)((params >> 8) & 0xFF);
  if (width < 1)
    width = 1;
  int msb = lsb + width - 1;
  if (msb > 31)
    msb = 31;
  int imm3 = (lsb >> 2) & 0x7;
  int imm2 = lsb & 0x3;
  /* Thumb-2 BFI: 11110 0 11 0110 Rn | 0 imm3 Rd imm2 0 msb */
  thumb_opcode op;
  op.size = 4;
  op.opcode = 0xF3600000 | ((uint32_t)rn << 16) | ((uint32_t)imm3 << 12) | ((uint32_t)rd << 8) | ((uint32_t)imm2 << 6) | (uint32_t)msb;
  ot(op);
  mach_writeback_dest(&dest, rd);
  mach_release_all(&ctx);
}

/* ============================================================
 * MachineOperand-based mul/div/mod/test-zero (_mop path)
 * ============================================================
 * Internal helpers and public entry point for 32-bit register-only ops:
 *   MUL, DIV, UDIV  — simple rd = rn OP rm
 *   IMOD, UMOD      — dest = src1 - (src1/src2)*src2
 *   TEST_ZERO       — CMP src, #0 (flags only, no dest)
 * MLA (accumulator) and UMULL (64-bit pair) remain on the old IR path.
 */

/* Emit rd = emitter(src1, src2) for register-only 3-operand ops. */
static void mach_regonly_binop_mop(MachineCodegenContext *ctx, const MachineOperand *src1, const MachineOperand *src2,
                                   const MachineOperand *dest, thumb_regonly3_handler_t emitter)
{
  /* 1. Get dest register (scratch if spill/param). */
  int dest_reg = mach_get_dest_reg(ctx, dest, 0);
  uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;

  /* Pre-exclude src2's physical register so that loading src1 (which may
   * need a scratch for deref) does not clobber src2's value. */
  if (src2->kind == MACH_OP_REG && !src2->needs_deref && thumb_is_hw_reg(src2->u.reg.r0))
    excl |= (1u << (uint32_t)src2->u.reg.r0);

  /* 2. Ensure src1 in a register; extend exclusion mask. */
  int src1_reg = mach_ensure_in_reg(ctx, src1, excl);
  if (thumb_is_hw_reg(src1_reg))
    excl |= (1u << (uint32_t)src1_reg);

  /* 3. Ensure src2 in a register. */
  int src2_reg = mach_ensure_in_reg(ctx, src2, excl);

  /* 4. Emit instruction. */
  ot_check(emitter((uint32_t)dest_reg, (uint32_t)src1_reg, (uint32_t)src2_reg));

  /* 5. Write result back to spill slot / stack param if needed. */
  mach_writeback_dest(dest, dest_reg);
}

/* Emit dest = src1 - (src1/src2)*src2 for IMOD/UMOD. */
static void mach_mod_mop(MachineCodegenContext *ctx, const MachineOperand *src1, const MachineOperand *src2,
                         const MachineOperand *dest, thumb_regonly3_handler_t div_emitter)
{
  /* 1. Get dest register. */
  int dest_reg = mach_get_dest_reg(ctx, dest, 0);
  uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;

  /* 2. Ensure src1 in a register. */
  int src1_reg = mach_ensure_in_reg(ctx, src1, excl);
  if (thumb_is_hw_reg(src1_reg))
    excl |= (1u << (uint32_t)src1_reg);

  /* 3. Ensure src2 in a register. */
  int src2_reg = mach_ensure_in_reg(ctx, src2, excl);
  if (thumb_is_hw_reg(src2_reg))
    excl |= (1u << (uint32_t)src2_reg);

  /* 4. Scratch register for quotient. */
  int quotient_reg = mach_alloc_scratch(ctx, excl);

  /* 5. quotient = src1 / src2 */
  ot_check(div_emitter((uint32_t)quotient_reg, (uint32_t)src1_reg, (uint32_t)src2_reg));

  /* 6. quotient = quotient * src2 */
  ot_check(thumb_mul_regonly((uint32_t)quotient_reg, (uint32_t)quotient_reg, (uint32_t)src2_reg));

  /* 7. dest = src1 - quotient */
  ot_check(th_sub_reg(dest_reg, src1_reg, quotient_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                      ENFORCE_ENCODING_NONE));

  /* 8. Write result back. */
  mach_writeback_dest(dest, dest_reg);
}

/* thumb_emit_mul64_mop
 * ============================================================
 * Emit a 64-bit multiply (lower 64 bits of the result) using MachineOperands.
 *
 * For a 64-bit result (dest->is_64bit):
 *   UMULL r_c_lo, r_c_hi, r_a_lo, r_b_lo  // a_lo * b_lo → 64-bit unsigned
 *   MLA   r_c_hi, r_a_hi, r_b_lo, r_c_hi  // cross product (when src1 is 64-bit)
 *   MLA   r_c_hi, r_a_lo, r_b_hi, r_c_hi  // cross product (when src2 is 64-bit)
 *
 * For a 32-bit result with 64-bit source(s):
 *   MUL   r_c, r_a_lo, r_b_lo             // upper bits don't contribute
 *
 * The lower 64 bits of the signed / unsigned 128-bit product are identical
 * (i.e. UMULL is correct for both signed and unsigned long long mul).
 * The caller must call mach_release_all() after this function returns.
 */
static void thumb_emit_mul64_mop(MachineCodegenContext *ctx, const MachineOperand *src1, const MachineOperand *src2,
                                 const MachineOperand *dest)
{
  uint32_t excl = 0;

  /* Resolve deref'd 64-bit sources before splitting into halves. */
  MachineOperand r_s1 = mach_resolve_deref_64(ctx, src1, &excl);
  MachineOperand r_s2 = mach_resolve_deref_64(ctx, src2, &excl);

  /* Load lo halves (always needed). */
  MachineOperand a_lo_op = r_s1.is_64bit ? mach_make_lo_half(&r_s1) : r_s1;
  a_lo_op.btype = IROP_BTYPE_INT32;
  a_lo_op.is_64bit = false;
  MachineOperand b_lo_op = r_s2.is_64bit ? mach_make_lo_half(&r_s2) : r_s2;
  b_lo_op.btype = IROP_BTYPE_INT32;
  b_lo_op.is_64bit = false;

  int r_a_lo = mach_ensure_in_reg(ctx, &a_lo_op, excl);
  if (thumb_is_hw_reg(r_a_lo))
    excl |= (1u << (uint32_t)r_a_lo);
  int r_b_lo = mach_ensure_in_reg(ctx, &b_lo_op, excl);
  if (thumb_is_hw_reg(r_b_lo))
    excl |= (1u << (uint32_t)r_b_lo);

  if (dest->is_64bit)
  {
    /* Load hi halves for cross-product MLA terms. */
    int r_a_hi = PREG_REG_NONE, r_b_hi = PREG_REG_NONE;
    if (r_s1.is_64bit)
    {
      MachineOperand a_hi_op = mach_make_hi_half(&r_s1);
      a_hi_op.btype = IROP_BTYPE_INT32;
      r_a_hi = mach_ensure_in_reg(ctx, &a_hi_op, excl);
      if (thumb_is_hw_reg(r_a_hi))
        excl |= (1u << (uint32_t)r_a_hi);
    }
    if (r_s2.is_64bit)
    {
      MachineOperand b_hi_op = mach_make_hi_half(&r_s2);
      b_hi_op.btype = IROP_BTYPE_INT32;
      r_b_hi = mach_ensure_in_reg(ctx, &b_hi_op, excl);
      if (thumb_is_hw_reg(r_b_hi))
        excl |= (1u << (uint32_t)r_b_hi);
    }

    /* Allocate 64-bit destination pair — must not overlap sources for UMULL. */
    MachineOperand dst_lo_op = mach_make_lo_half(dest);
    dst_lo_op.btype = IROP_BTYPE_INT32;
    MachineOperand dst_hi_op = mach_make_hi_half(dest);
    dst_hi_op.btype = IROP_BTYPE_INT32;

    int r_c_lo = mach_get_dest_reg(ctx, &dst_lo_op, excl);
    if (thumb_is_hw_reg(r_c_lo))
      excl |= (1u << (uint32_t)r_c_lo);
    int r_c_hi = mach_get_dest_reg(ctx, &dst_hi_op, excl);

    /* UMULL: r_c_lo:r_c_hi = r_a_lo * r_b_lo (unsigned 64-bit product) */
    ot_check(th_umull((uint32_t)r_c_lo, (uint32_t)r_c_hi, (uint32_t)r_a_lo, (uint32_t)r_b_lo));

    /* Add cross products to high half. */
    if (thumb_is_hw_reg(r_a_hi))
      ot_check(th_mla((uint32_t)r_c_hi, (uint32_t)r_a_hi, (uint32_t)r_b_lo, (uint32_t)r_c_hi));
    if (thumb_is_hw_reg(r_b_hi))
      ot_check(th_mla((uint32_t)r_c_hi, (uint32_t)r_a_lo, (uint32_t)r_b_hi, (uint32_t)r_c_hi));

    mach_writeback_dest(&dst_lo_op, r_c_lo);
    mach_writeback_dest(&dst_hi_op, r_c_hi);
  }
  else
  {
    /* 32-bit result with 64-bit source(s): only the low bits matter. */
    MachineOperand dest32 = *dest;
    dest32.is_64bit = false;
    int r_c = mach_get_dest_reg(ctx, &dest32, excl);
    ot_check(thumb_mul_regonly((uint32_t)r_c, (uint32_t)r_a_lo, (uint32_t)r_b_lo));
    mach_writeback_dest(&dest32, r_c);
  }
}

/* tcc_gen_machine_muldiv_mop: MachineOperand-based entry point for multiply,
 * divide, modulo, and test-zero operations.  Called from ir/codegen.c when
 * use_mop_muldiv is true for:
 *   MUL                         — 32-bit or 64-bit multiply
 *   DIV, UDIV, IMOD, UMOD       — 32-bit divide/modulo
 *   TEST_ZERO                   — 32-bit or 64-bit compare against zero (flags only)
 * MLA (accumulator; 4-operand) uses tcc_gen_machine_mla_mop.
 * UMULL (64-bit output from 32-bit inputs) uses tcc_gen_machine_umull_mop.
 */
/* Decompose multiply-by-constant into shift+add sequences.
 * Returns 1 if handled, 0 to fall back to hardware MUL. */
static int thumb_try_mul_by_const_mop(MachineCodegenContext *ctx, MachineOperand *src1, MachineOperand *src2,
                                      MachineOperand *dest)
{
  /* Identify which operand is the immediate and which is the variable. */
  const MachineOperand *imm_op, *var_op;
  if (src2->kind == MACH_OP_IMM)
  {
    imm_op = src2;
    var_op = src1;
  }
  else if (src1->kind == MACH_OP_IMM)
  {
    imm_op = src1;
    var_op = src2;
  }
  else
    return 0;

  int64_t c = imm_op->u.imm.val;
  if (c <= 0)
    return 0;

  /* Determine the decomposition pattern.
   * We handle: powers of 2, (2^n ± 1), and products thereof.
   *
   * Pattern             Insns  Example
   * ─────────────────── ───── ───────
   * 2^n                 1     LSL Rd, Rn, #n
   * 2^n + 1             1     ADD Rd, Rn, Rn LSL #n
   * 2^n - 1             1     SUB Rd, Rn LSL #n, Rn  (RSB-like via SUB)
   * (2^a + 1) * 2^b     2     ADD Rd, Rn, Rn LSL #a; LSL Rd, Rd, #b
   * (2^a - 1) * 2^b     2     SUB Rd, Rn LSL #a, Rn; LSL Rd, Rd, #b
   * (2^a + 1)(2^b + 1)  2     ADD Rd, Rn, Rn LSL #a; ADD Rd, Rd, Rn LSL #(a+b)
   *                            — only some cases, handled via table
   */

  int shift1 = 0, shift2 = 0;
  enum
  {
    MUL_NONE,
    MUL_POWER_OF_2,          /* c = 2^n : LSL #n */
    MUL_TWO_N_PLUS_1,        /* c = 2^n+1 : ADD Rd, Rn, Rn LSL #n */
    MUL_TWO_N_MINUS_1,       /* c = 2^n-1 : SUB Rd, Rn LSL #n, Rn */
    MUL_TWO_N_PLUS_1_SHIFT,  /* c = (2^a+1)*2^b : ADD; LSL */
    MUL_TWO_N_MINUS_1_SHIFT, /* c = (2^a-1)*2^b : SUB; LSL */
  } pattern = MUL_NONE;

  /* Check for power of 2 */
  if (c > 0 && (c & (c - 1)) == 0)
  {
    int n = 0;
    int64_t v = c;
    while (v > 1)
    {
      n++;
      v >>= 1;
    }
    if (n >= 1 && n <= 31)
    {
      shift1 = n;
      pattern = MUL_POWER_OF_2;
    }
  }

  /* Check for 2^n + 1 (3, 5, 9, 17, ...) */
  if (pattern == MUL_NONE && c >= 3)
  {
    int64_t v = c - 1;
    if (v > 0 && (v & (v - 1)) == 0)
    {
      int n = 0;
      while (v > 1)
      {
        n++;
        v >>= 1;
      }
      if (n >= 1 && n <= 31)
      {
        shift1 = n;
        pattern = MUL_TWO_N_PLUS_1;
      }
    }
  }

  /* Check for 2^n - 1 (7, 15, 31, ...) */
  if (pattern == MUL_NONE && c >= 7)
  {
    int64_t v = c + 1;
    if (v > 0 && (v & (v - 1)) == 0)
    {
      int n = 0;
      while (v > 1)
      {
        n++;
        v >>= 1;
      }
      if (n >= 2 && n <= 31)
      {
        shift1 = n;
        pattern = MUL_TWO_N_MINUS_1;
      }
    }
  }

  /* Check for (2^a + 1) * 2^b (6, 10, 12, 20, 24, 40, 48, ...) */
  if (pattern == MUL_NONE && c >= 6)
  {
    int64_t v = c;
    int b = 0;
    while ((v & 1) == 0)
    {
      b++;
      v >>= 1;
    }
    if (b >= 1 && b <= 31)
    {
      int64_t inner = v - 1;
      if (inner > 0 && (inner & (inner - 1)) == 0)
      {
        int a = 0;
        while (inner > 1)
        {
          a++;
          inner >>= 1;
        }
        if (a >= 1 && a <= 31)
        {
          shift1 = a;
          shift2 = b;
          pattern = MUL_TWO_N_PLUS_1_SHIFT;
        }
      }
    }
  }

  /* Check for (2^a - 1) * 2^b (14, 28, 30, 56, 60, 62, ...) */
  if (pattern == MUL_NONE && c >= 14)
  {
    int64_t v = c;
    int b = 0;
    while ((v & 1) == 0)
    {
      b++;
      v >>= 1;
    }
    if (b >= 1 && b <= 31)
    {
      int64_t inner = v + 1;
      if (inner > 0 && (inner & (inner - 1)) == 0)
      {
        int a = 0;
        while (inner > 1)
        {
          a++;
          inner >>= 1;
        }
        if (a >= 2 && a <= 31)
        {
          shift1 = a;
          shift2 = b;
          pattern = MUL_TWO_N_MINUS_1_SHIFT;
        }
      }
    }
  }

  if (pattern == MUL_NONE)
    return 0;

  /* Emit the decomposed sequence. */
  int dest_reg = mach_get_dest_reg(ctx, dest, 0);
  uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;
  int var_reg = mach_ensure_in_reg(ctx, var_op, excl);
  thumb_flags_behaviour fl = flags_safe();
  thumb_shift sh;

  switch (pattern)
  {
  case MUL_POWER_OF_2:
    ot_check(th_lsl_imm((uint32_t)dest_reg, (uint32_t)var_reg, (uint32_t)shift1, fl, ENFORCE_ENCODING_NONE));
    break;

  case MUL_TWO_N_PLUS_1:
    sh = (thumb_shift){THUMB_SHIFT_LSL, (uint16_t)shift1, THUMB_SHIFT_IMMEDIATE};
    ot_check(th_add_reg((uint32_t)dest_reg, (uint32_t)var_reg, (uint32_t)var_reg, fl, sh, ENFORCE_ENCODING_NONE));
    break;

  case MUL_TWO_N_MINUS_1:
    /* Thumb-2 SUB Rd, Rn, Rm LSL #n = Rn - (Rm << n).
     * We need (var << n) - var, which is the reverse. No RSB with shift
     * exists in Thumb-2, so we do: LSL Rd, var, #n; SUB Rd, Rd, var. */
    ot_check(th_lsl_imm((uint32_t)dest_reg, (uint32_t)var_reg, (uint32_t)shift1, fl, ENFORCE_ENCODING_NONE));
    sh = (thumb_shift){THUMB_SHIFT_NONE, 0, THUMB_SHIFT_IMMEDIATE};
    ot_check(th_sub_reg((uint32_t)dest_reg, (uint32_t)dest_reg, (uint32_t)var_reg, fl, sh, ENFORCE_ENCODING_NONE));
    break;

  case MUL_TWO_N_PLUS_1_SHIFT:
    sh = (thumb_shift){THUMB_SHIFT_LSL, (uint16_t)shift1, THUMB_SHIFT_IMMEDIATE};
    ot_check(th_add_reg((uint32_t)dest_reg, (uint32_t)var_reg, (uint32_t)var_reg, fl, sh, ENFORCE_ENCODING_NONE));
    ot_check(th_lsl_imm((uint32_t)dest_reg, (uint32_t)dest_reg, (uint32_t)shift2, fl, ENFORCE_ENCODING_NONE));
    break;

  case MUL_TWO_N_MINUS_1_SHIFT:
    /* (2^a - 1) * 2^b: LSL Rd, var, #a; SUB Rd, Rd, var; LSL Rd, Rd, #b */
    ot_check(th_lsl_imm((uint32_t)dest_reg, (uint32_t)var_reg, (uint32_t)shift1, fl, ENFORCE_ENCODING_NONE));
    sh = (thumb_shift){THUMB_SHIFT_NONE, 0, THUMB_SHIFT_IMMEDIATE};
    ot_check(th_sub_reg((uint32_t)dest_reg, (uint32_t)dest_reg, (uint32_t)var_reg, fl, sh, ENFORCE_ENCODING_NONE));
    ot_check(th_lsl_imm((uint32_t)dest_reg, (uint32_t)dest_reg, (uint32_t)shift2, fl, ENFORCE_ENCODING_NONE));
    break;

  default:
    return 0;
  }

  mach_writeback_dest(dest, dest_reg);
  mach_release_all(ctx);
  return 1;
}

/* Fused MUL-by-const + ADD peephole.
 * Transforms:  tmp = var * C;  dest = base + tmp
 * Into a shorter sequence using ARM shifted-add (ADD Rd, Rn, Rm LSL #imm):
 *   C = 2^n:              ADD dest, base, var LSL #n           (1 insn vs 2)
 *   C = (2^a+1)*2^b:      ADD t, var, var LSL #a;
 *                          ADD dest, base, t LSL #b             (2 insn vs 3)
 *   C = (2^a-1)*2^b:      LSL t, var, #a; SUB t, t, var;
 *                          ADD dest, base, t LSL #b             (3 insn vs 4)
 * Returns 1 if fused, 0 to fall back to separate MUL + ADD. */
ST_FUNC int tcc_gen_machine_mul_const_add_fused_mop(MachineOperand mul_var, int64_t mul_const,
                                                    MachineOperand mul_dest, MachineOperand add_base,
                                                    MachineOperand add_dest)
{
  if (mul_const <= 0)
    return 0;

  int shift1 = 0, shift2 = 0;
  enum
  {
    FUSE_NONE,
    FUSE_POW2,
    FUSE_TWO_N_PLUS_1_SHIFT,
    FUSE_TWO_N_MINUS_1_SHIFT,
  } pattern = FUSE_NONE;

  /* Power of 2: C = 2^n */
  if (mul_const > 1 && (mul_const & (mul_const - 1)) == 0)
  {
    int n = 0;
    int64_t v = mul_const;
    while (v > 1) { n++; v >>= 1; }
    if (n >= 1 && n <= 31)
    {
      shift1 = n;
      pattern = FUSE_POW2;
    }
  }

  /* (2^a + 1) * 2^b: e.g. 12 = 3*4 = (2^1+1)*2^2 */
  if (pattern == FUSE_NONE && mul_const >= 6)
  {
    int64_t v = mul_const;
    int b = 0;
    while ((v & 1) == 0) { b++; v >>= 1; }
    if (b >= 1 && b <= 31)
    {
      int64_t inner = v - 1;
      if (inner > 0 && (inner & (inner - 1)) == 0)
      {
        int a = 0;
        while (inner > 1) { a++; inner >>= 1; }
        if (a >= 1 && a <= 31)
        {
          shift1 = a;
          shift2 = b;
          pattern = FUSE_TWO_N_PLUS_1_SHIFT;
        }
      }
    }
  }

  /* (2^a - 1) * 2^b: e.g. 28 = 7*4 = (2^3-1)*2^2 */
  if (pattern == FUSE_NONE && mul_const >= 14)
  {
    int64_t v = mul_const;
    int b = 0;
    while ((v & 1) == 0) { b++; v >>= 1; }
    if (b >= 1 && b <= 31)
    {
      int64_t inner = v + 1;
      if (inner > 0 && (inner & (inner - 1)) == 0)
      {
        int a = 0;
        while (inner > 1) { a++; inner >>= 1; }
        if (a >= 2 && a <= 31)
        {
          shift1 = a;
          shift2 = b;
          pattern = FUSE_TWO_N_MINUS_1_SHIFT;
        }
      }
    }
  }

  if (pattern == FUSE_NONE)
    return 0;

  MachineCodegenContext ctx = {0};
  thumb_flags_behaviour fl = flags_safe();
  thumb_shift sh;

  /* Allocate registers: dest first (may hint to the ADD dest's phys reg),
   * then base and var, using exclusion masks to prevent conflicts. */
  int dest_reg = mach_get_dest_reg(&ctx, &add_dest, 0);
  uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;
  int base_reg = mach_ensure_in_reg(&ctx, &add_base, excl);
  if (thumb_is_hw_reg(base_reg))
    excl |= (1u << (uint32_t)base_reg);
  int var_reg = mach_ensure_in_reg(&ctx, &mul_var, excl);

  switch (pattern)
  {
  case FUSE_POW2:
    /* ADD dest, base, var LSL #n */
    sh = (thumb_shift){THUMB_SHIFT_LSL, (uint16_t)shift1, THUMB_SHIFT_IMMEDIATE};
    ot_check(th_add_reg((uint32_t)dest_reg, (uint32_t)base_reg, (uint32_t)var_reg, fl, sh, ENFORCE_ENCODING_NONE));
    break;

  case FUSE_TWO_N_PLUS_1_SHIFT:
  {
    /* Step 1: ADD tmp, var, var LSL #a */
    if (thumb_is_hw_reg(var_reg))
      excl |= (1u << (uint32_t)var_reg);
    int tmp_reg = mach_get_dest_reg(&ctx, &mul_dest, excl);
    sh = (thumb_shift){THUMB_SHIFT_LSL, (uint16_t)shift1, THUMB_SHIFT_IMMEDIATE};
    ot_check(th_add_reg((uint32_t)tmp_reg, (uint32_t)var_reg, (uint32_t)var_reg, fl, sh, ENFORCE_ENCODING_NONE));
    /* Step 2: ADD dest, base, tmp LSL #b */
    sh = (thumb_shift){THUMB_SHIFT_LSL, (uint16_t)shift2, THUMB_SHIFT_IMMEDIATE};
    ot_check(th_add_reg((uint32_t)dest_reg, (uint32_t)base_reg, (uint32_t)tmp_reg, fl, sh, ENFORCE_ENCODING_NONE));
    break;
  }

  case FUSE_TWO_N_MINUS_1_SHIFT:
  {
    /* Step 1: LSL tmp, var, #a */
    if (thumb_is_hw_reg(var_reg))
      excl |= (1u << (uint32_t)var_reg);
    int tmp_reg = mach_get_dest_reg(&ctx, &mul_dest, excl);
    ot_check(th_lsl_imm((uint32_t)tmp_reg, (uint32_t)var_reg, (uint32_t)shift1, fl, ENFORCE_ENCODING_NONE));
    /* Step 2: SUB tmp, tmp, var */
    sh = (thumb_shift){THUMB_SHIFT_NONE, 0, THUMB_SHIFT_IMMEDIATE};
    ot_check(th_sub_reg((uint32_t)tmp_reg, (uint32_t)tmp_reg, (uint32_t)var_reg, fl, sh, ENFORCE_ENCODING_NONE));
    /* Step 3: ADD dest, base, tmp LSL #b */
    sh = (thumb_shift){THUMB_SHIFT_LSL, (uint16_t)shift2, THUMB_SHIFT_IMMEDIATE};
    ot_check(th_add_reg((uint32_t)dest_reg, (uint32_t)base_reg, (uint32_t)tmp_reg, fl, sh, ENFORCE_ENCODING_NONE));
    break;
  }

  default:
    mach_release_all(&ctx);
    return 0;
  }

  mach_writeback_dest(&add_dest, dest_reg);
  mach_release_all(&ctx);
  return 1;
}

ST_FUNC void tcc_gen_machine_muldiv_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op)
{
  MachineCodegenContext ctx = {0};
  switch (op)
  {
  case TCCIR_OP_MUL:
    if (src1.is_64bit || src2.is_64bit || dest.is_64bit)
      thumb_emit_mul64_mop(&ctx, &src1, &src2, &dest);
    else if (!thumb_try_mul_by_const_mop(&ctx, &src1, &src2, &dest))
      mach_regonly_binop_mop(&ctx, &src1, &src2, &dest, thumb_mul_regonly);
    break;
  case TCCIR_OP_DIV:
    mach_regonly_binop_mop(&ctx, &src1, &src2, &dest, thumb_sdiv_regonly);
    break;
  case TCCIR_OP_UDIV:
    mach_regonly_binop_mop(&ctx, &src1, &src2, &dest, thumb_udiv_regonly);
    break;
  case TCCIR_OP_IMOD:
    mach_mod_mop(&ctx, &src1, &src2, &dest, thumb_sdiv_regonly);
    break;
  case TCCIR_OP_UMOD:
    mach_mod_mop(&ctx, &src1, &src2, &dest, thumb_udiv_regonly);
    break;
  case TCCIR_OP_TEST_ZERO:
  {
    if (src1.is_64bit)
    {
      /* 64-bit: Z set iff (lo == 0 && hi == 0).
       * Use CMP lo,#0; IT EQ; CMPEQ hi,#0 to avoid clobbering source registers. */
      uint32_t excl = 0;
      MachineOperand resolved = mach_resolve_deref_64(&ctx, &src1, &excl);
      MachineOperand lo = mach_make_lo_half(&resolved);
      lo.btype = IROP_BTYPE_INT32;
      MachineOperand hi = mach_make_hi_half(&resolved);
      hi.btype = IROP_BTYPE_INT32;
      int r_lo = mach_ensure_in_reg(&ctx, &lo, excl);
      if (thumb_is_hw_reg(r_lo))
        excl |= (1u << (uint32_t)r_lo);
      int r_hi = mach_ensure_in_reg(&ctx, &hi, excl);
      ot_check(th_cmp_imm(r_lo, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
      th_literal_pool_reserve_upcoming_bytes(6);
      ot_check(th_it(mapcc(TOK_EQ), 0x8)); /* IT EQ (single instruction) */
      ot_check(th_cmp_imm(r_hi, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
    }
    else
    {
      /* 32-bit: CMP src, #0 — no destination, only flags. */
      int src_reg = mach_ensure_in_reg(&ctx, &src1, 0);
      ot_check(th_cmp_imm(src_reg, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
    }
    break;
  }
  default:
    tcc_error("compiler_error: tcc_gen_machine_muldiv_mop: unhandled op %d", (int)op);
    break;
  }
  mach_release_all(&ctx);
}

/* tcc_gen_machine_cmp_eq64_mop: 64-bit equality comparison.
 * Emits CMP hi1,hi2; IT EQ; CMPEQ lo1,lo2 which correctly sets
 * the Z flag for full 64-bit equality (used by SETIF/JUMPIF EQ/NE). */
ST_FUNC void tcc_gen_machine_cmp_eq64_mop(MachineOperand src1, MachineOperand src2)
{
  MachineCodegenContext ctx = {0};
  uint32_t excl = 0;

  if (src1.kind == MACH_OP_REG)
  {
    if (src1.u.reg.r0 != (int)PREG_REG_NONE)
      excl |= (1u << (uint32_t)src1.u.reg.r0);
    if (!src1.needs_deref && src1.is_64bit && src1.u.reg.r1 >= 0)
      excl |= (1u << (uint32_t)src1.u.reg.r1);
  }
  if (src2.kind == MACH_OP_REG)
  {
    if (src2.u.reg.r0 != (int)PREG_REG_NONE)
      excl |= (1u << (uint32_t)src2.u.reg.r0);
    if (!src2.needs_deref && src2.is_64bit && src2.u.reg.r1 >= 0)
      excl |= (1u << (uint32_t)src2.u.reg.r1);
  }

  MachineOperand r_src1 = mach_resolve_deref_64(&ctx, &src1, &excl);
  MachineOperand r_src2 = mach_resolve_deref_64(&ctx, &src2, &excl);

  MachineOperand s1_lo = mach_make_lo_half(&r_src1);
  s1_lo.btype = IROP_BTYPE_INT32;
  int rn_lo = mach_ensure_in_reg(&ctx, &s1_lo, excl);
  if (thumb_is_hw_reg(rn_lo))
    excl |= (1u << (uint32_t)rn_lo);

  MachineOperand s1_hi = mach_make_hi_half(&r_src1);
  s1_hi.btype = IROP_BTYPE_INT32;
  int rn_hi = mach_ensure_in_reg(&ctx, &s1_hi, excl);
  if (thumb_is_hw_reg(rn_hi))
    excl |= (1u << (uint32_t)rn_hi);

  /* Immediate-CMP fast path: if src2 is a u64 immediate, try the cmp-imm
   * form (`cmp.w Rn, #imm`) for each half — avoids loading the constant
   * into a scratch reg.  Probe encodability before allocating scratches:
   * `mach_ensure_in_reg` on a MACH_OP_IMM would unconditionally emit a
   * `movs Rscratch, #imm`, which is exactly the instruction we're trying
   * to avoid here. */
  thumb_opcode hi_imm_op = {0};
  thumb_opcode lo_imm_op = {0};
  if (r_src2.kind == MACH_OP_IMM)
  {
    const uint64_t imm = (uint64_t)r_src2.u.imm.val;
    const uint32_t imm_lo = (uint32_t)(imm & 0xffffffffu);
    const uint32_t imm_hi = (uint32_t)(imm >> 32);
    hi_imm_op = th_cmp_imm((uint32_t)rn_hi, imm_hi, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
    lo_imm_op = th_cmp_imm((uint32_t)rn_lo, imm_lo, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  }

  /* Use cmp-imm for whichever halves fit; only allocate scratch
   * registers for halves that need them. */
  int hi_uses_imm = (r_src2.kind == MACH_OP_IMM && hi_imm_op.size);
  int lo_uses_imm = (r_src2.kind == MACH_OP_IMM && lo_imm_op.size);

  int rm_lo = 0, rm_hi = 0;
  if (!lo_uses_imm)
  {
    MachineOperand s2_lo = mach_make_lo_half(&r_src2);
    s2_lo.btype = IROP_BTYPE_INT32;
    rm_lo = mach_ensure_in_reg(&ctx, &s2_lo, excl);
    if (thumb_is_hw_reg(rm_lo))
      excl |= (1u << (uint32_t)rm_lo);
  }
  if (!hi_uses_imm)
  {
    MachineOperand s2_hi = mach_make_hi_half(&r_src2);
    s2_hi.btype = IROP_BTYPE_INT32;
    rm_hi = mach_ensure_in_reg(&ctx, &s2_hi, excl);
  }

  if (hi_uses_imm)
    ot_check(hi_imm_op);
  else
    ot_check(th_cmp_reg(0, (uint32_t)rn_hi, (uint32_t)rm_hi, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT,
                         ENFORCE_ENCODING_NONE));
  th_literal_pool_reserve_upcoming_bytes(6);
  ot_check(th_it(mapcc(TOK_EQ), 0x8));
  if (lo_uses_imm)
    ot_check(lo_imm_op);
  else
    ot_check(th_cmp_reg(0, (uint32_t)rn_lo, (uint32_t)rm_lo, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT,
                         ENFORCE_ENCODING_NONE));

  mach_release_all(&ctx);
}

/* tcc_gen_machine_subs_eq_select_01: emit
 *   SUBS dest, src1, #K
 *   IT NE
 *   MOVNE dest, #1
 * for the CMP src1,#K + SELECT(#1,#0,NE) / SELECT(#0,#1,EQ) peephole.
 * Returns 1 if emitted, 0 if the SUBS immediate didn't encode (caller falls back). */
ST_FUNC int tcc_gen_machine_subs_eq_select_01(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  if (src2.kind != MACH_OP_IMM)
    return 0;
  if (src1.kind != MACH_OP_REG || src1.needs_deref || src1.u.reg.r0 < 0)
    return 0;
  if (dest.kind != MACH_OP_REG || dest.needs_deref || dest.u.reg.r0 < 0)
    return 0;

  uint32_t src_reg = (uint32_t)src1.u.reg.r0;
  uint32_t dst_reg = (uint32_t)dest.u.reg.r0;
  uint32_t Ku = (uint32_t)src2.u.imm.val;

  thumb_opcode subs = th_sub_imm(dst_reg, src_reg, Ku, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE);
  if (subs.size == 0)
    return 0;

  /* Reserve so a literal-pool flush can't split the IT/MOV pair. */
  th_literal_pool_reserve_upcoming_bytes(10);
  ot_check(subs);
  ot_check(th_it(mapcc(TOK_NE), 0x8u));
  thumb_opcode movne = th_generic_mov_imm(dst_reg, 1);
  if (movne.size != 0) {
    ot_check(movne);
  } else {
    /* mov #1 always encodes on ARM Thumb-2, but be safe. */
    load_full_const((int)dst_reg, PREG_NONE, 1u, 0u);
  }
  return 1;
}

/* tcc_gen_machine_mla_mop: MachineOperand-based entry point for MLA.
 * dest = src1 * src2 + accum  (all operands are 32-bit)
 *
 * All four operands are loaded into hardware registers via mach_ensure_in_reg
 * before emitting a single MLA instruction.  No fallback path is needed
 * because mach_ensure_in_reg always returns a valid register.
 *
 * Note: th_mla(rd, rn, rm, ra) → rd = rn * rm + ra
 */
ST_FUNC void tcc_gen_machine_mla_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest,
                                     MachineOperand accum)
{
  MachineCodegenContext ctx = {0};

  /* Pre-exclude registers directly referenced by REG operands so that scratch
   * allocations for other operands (e.g. immediates) cannot clobber them. */
  uint32_t live_regs = 0;
  if (src1.kind == MACH_OP_REG && !src1.needs_deref)
    live_regs |= (1u << (uint32_t)src1.u.reg.r0);
  if (src2.kind == MACH_OP_REG && !src2.needs_deref)
    live_regs |= (1u << (uint32_t)src2.u.reg.r0);
  if (accum.kind == MACH_OP_REG && !accum.needs_deref)
    live_regs |= (1u << (uint32_t)accum.u.reg.r0);

  int src1_reg = mach_ensure_in_reg(&ctx, &src1, live_regs);
  uint32_t excl = live_regs;
  if (thumb_is_hw_reg(src1_reg))
    excl |= (1u << (uint32_t)src1_reg);

  int src2_reg = mach_ensure_in_reg(&ctx, &src2, excl);
  if (thumb_is_hw_reg(src2_reg))
    excl |= (1u << (uint32_t)src2_reg);

  int accum_reg = mach_ensure_in_reg(&ctx, &accum, excl);
  if (thumb_is_hw_reg(accum_reg))
    excl |= (1u << (uint32_t)accum_reg);

  int dest_reg = mach_get_dest_reg(&ctx, &dest, excl);

  /* th_mla(rd, rn, rm, ra): rd = rn * rm + ra */
  ot_check(th_mla((uint32_t)dest_reg, (uint32_t)src1_reg, (uint32_t)src2_reg, (uint32_t)accum_reg));

  mach_writeback_dest(&dest, dest_reg);
  mach_release_all(&ctx);
}

/* tcc_gen_machine_umull_mop: MachineOperand-based entry point for UMULL.
 * {dest_hi:dest_lo} = (uint32_t)src1 * (uint32_t)src2  (64-bit unsigned result)
 *
 * src1 and src2 are 32-bit inputs (is_64bit is cleared before loading).
 * dest must be a 64-bit pair; it is split via mach_make_lo/hi_half.
 * Each half is allocated independently via mach_get_dest_reg, with the
 * exclusion mask preventing rdlo==rdhi and preventing overlap with rn/rm.
 *
 * Note: th_umull(rdlo, rdhi, rn, rm) → {rdhi:rdlo} = rn * rm (unsigned)
 */
ST_FUNC void tcc_gen_machine_umull_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  MachineCodegenContext ctx = {0};

  /* UMULL takes 32-bit inputs — drop any 64-bit flag the src may carry. */
  MachineOperand s1 = src1;
  s1.is_64bit = false;
  MachineOperand s2 = src2;
  s2.is_64bit = false;

  int rn = mach_ensure_in_reg(&ctx, &s1, 0);
  uint32_t excl = thumb_is_hw_reg(rn) ? (1u << (uint32_t)rn) : 0u;

  int rm = mach_ensure_in_reg(&ctx, &s2, excl);
  if (thumb_is_hw_reg(rm))
    excl |= (1u << (uint32_t)rm);

  /* Split 64-bit destination into lo (bits [31:0]) and hi (bits [63:32]). */
  MachineOperand dst_lo = mach_make_lo_half(&dest);
  MachineOperand dst_hi = mach_make_hi_half(&dest);
  dst_lo.btype = IROP_BTYPE_INT32;
  dst_hi.btype = IROP_BTYPE_INT32;

  int rd_lo = mach_get_dest_reg(&ctx, &dst_lo, excl);
  if (thumb_is_hw_reg(rd_lo))
    excl |= (1u << (uint32_t)rd_lo);
  int rd_hi = mach_get_dest_reg(&ctx, &dst_hi, excl);

  /* th_umull(rdlo, rdhi, rn, rm): {rdhi:rdlo} = rn * rm */
  ot_check(th_umull((uint32_t)rd_lo, (uint32_t)rd_hi, (uint32_t)rn, (uint32_t)rm));

  mach_writeback_dest(&dst_lo, rd_lo);
  mach_writeback_dest(&dst_hi, rd_hi);
  mach_release_all(&ctx);
}

/* tcc_gen_machine_smull_mop: MachineOperand-based entry point for SMULL.
 * {dest_hi:dest_lo} = (int32_t)src1 * (int32_t)src2  (64-bit signed result).
 * Mirrors umull_mop but emits th_smull. */
ST_FUNC void tcc_gen_machine_smull_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  MachineCodegenContext ctx = {0};

  MachineOperand s1 = src1;
  s1.is_64bit = false;
  MachineOperand s2 = src2;
  s2.is_64bit = false;

  int rn = mach_ensure_in_reg(&ctx, &s1, 0);
  uint32_t excl = thumb_is_hw_reg(rn) ? (1u << (uint32_t)rn) : 0u;

  int rm = mach_ensure_in_reg(&ctx, &s2, excl);
  if (thumb_is_hw_reg(rm))
    excl |= (1u << (uint32_t)rm);

  MachineOperand dst_lo = mach_make_lo_half(&dest);
  MachineOperand dst_hi = mach_make_hi_half(&dest);
  dst_lo.btype = IROP_BTYPE_INT32;
  dst_hi.btype = IROP_BTYPE_INT32;

  int rd_lo = mach_get_dest_reg(&ctx, &dst_lo, excl);
  if (thumb_is_hw_reg(rd_lo))
    excl |= (1u << (uint32_t)rd_lo);
  int rd_hi = mach_get_dest_reg(&ctx, &dst_hi, excl);

  /* th_smull(rdlo, rdhi, rn, rm): {rdhi:rdlo} = (signed)rn * (signed)rm */
  ot_check(th_smull((uint32_t)rd_lo, (uint32_t)rd_hi, (uint32_t)rn, (uint32_t)rm));

  mach_writeback_dest(&dst_lo, rd_lo);
  mach_writeback_dest(&dst_hi, rd_hi);
  mach_release_all(&ctx);
}

/* tcc_gen_machine_mlal_accum_mop: emit SMLAL/UMLAL for
 *   dest = accum + (int32/uint32)src1 * (int32/uint32)src2
 *
 * This narrow helper is used by codegen peepholes after register allocation.
 * It only handles the cheap in-place accumulate form, where the ADD destination
 * already holds the accumulator pair.  Other forms fall back to SMULL/UMULL
 * plus the normal 64-bit ADD so we do not risk clobbering multiply sources. */
ST_FUNC int tcc_gen_machine_mlal_accum_mop(MachineOperand src1, MachineOperand src2, MachineOperand accum,
                                           MachineOperand dest, int is_signed)
{
  if (!dest.is_64bit || !accum.is_64bit)
    return 0;
  if (dest.kind != MACH_OP_REG || accum.kind != MACH_OP_REG)
    return 0;
  if (dest.needs_deref || accum.needs_deref)
    return 0;
  if (dest.u.reg.r0 != accum.u.reg.r0 || dest.u.reg.r1 != accum.u.reg.r1)
    return 0;

  int rd_lo = dest.u.reg.r0;
  int rd_hi = dest.u.reg.r1;
  if (!thumb_is_hw_reg(rd_lo) || !thumb_is_hw_reg(rd_hi) || rd_lo == rd_hi)
    return 0;

  MachineCodegenContext ctx = {0};
  MachineOperand s1 = src1;
  s1.is_64bit = false;
  MachineOperand s2 = src2;
  s2.is_64bit = false;

  uint32_t excl = (1u << (uint32_t)rd_lo) | (1u << (uint32_t)rd_hi);
  int rn = mach_ensure_in_reg(&ctx, &s1, excl);
  if (thumb_is_hw_reg(rn))
    excl |= (1u << (uint32_t)rn);

  int rm = mach_ensure_in_reg(&ctx, &s2, excl);
  if (thumb_is_hw_reg(rm))
    excl |= (1u << (uint32_t)rm);

  if (is_signed)
    ot_check(th_smlal((uint32_t)rd_lo, (uint32_t)rd_hi, (uint32_t)rn, (uint32_t)rm));
  else
    ot_check(th_umlal((uint32_t)rd_lo, (uint32_t)rd_hi, (uint32_t)rn, (uint32_t)rm));

  MachineOperand dst_lo = mach_make_lo_half(&dest);
  MachineOperand dst_hi = mach_make_hi_half(&dest);
  dst_lo.btype = IROP_BTYPE_INT32;
  dst_hi.btype = IROP_BTYPE_INT32;
  mach_writeback_dest(&dst_lo, rd_lo);
  mach_writeback_dest(&dst_hi, rd_hi);
  mach_release_all(&ctx);
  return 1;
}

/* tcc_gen_machine_pack64_mop: lower TCCIR_OP_PACK64 by emitting two
 * 32-bit assigns into the dest's halves.  src_lo and src_hi are u32
 * operands; dest is a u64 register pair / spill / param slot.
 *
 * The two sub-assigns delegate to tcc_gen_machine_assign_mop, so they
 * benefit from its existing handling of every dest kind (REG/SPILL/...).
 * Often regalloc has already aligned the registers (e.g. dest.r0 = src_lo
 * register), in which case the sub-assigns degrade to a no-op MOV that
 * the encoder can skip. */
ST_FUNC void tcc_gen_machine_pack64_mop(MachineOperand src_lo, MachineOperand src_hi, MachineOperand dest)
{
  if (!dest.is_64bit)
  {
    tcc_error("compiler_error: tcc_gen_machine_pack64_mop: dest not 64-bit");
    return;
  }
  MachineOperand dst_lo = mach_make_lo_half(&dest);
  MachineOperand dst_hi = mach_make_hi_half(&dest);
  dst_lo.btype = IROP_BTYPE_INT32;
  dst_hi.btype = IROP_BTYPE_INT32;

  /* Detect register-swap aliasing: dst_lo == src_hi AND dst_hi == src_lo.
   * Neither write order can preserve both source values; we must stage one
   * side through a scratch register. */
  int swap_alias = 0;
  if (src_lo.kind == MACH_OP_REG && !src_lo.needs_deref &&
      src_hi.kind == MACH_OP_REG && !src_hi.needs_deref &&
      dst_lo.kind == MACH_OP_REG && !dst_lo.needs_deref &&
      dst_hi.kind == MACH_OP_REG && !dst_hi.needs_deref &&
      src_hi.u.reg.r0 == dst_lo.u.reg.r0 && src_lo.u.reg.r0 == dst_hi.u.reg.r0 &&
      src_lo.u.reg.r0 != src_hi.u.reg.r0)
    swap_alias = 1;

  if (swap_alias)
  {
    /* Save src_lo to a scratch before overwriting it via dst_hi. */
    uint32_t excl = (1u << (uint32_t)dst_lo.u.reg.r0) | (1u << (uint32_t)dst_hi.u.reg.r0);
    ScratchRegAlloc scratch = get_scratch_reg_with_save(excl);
    ot_check_mov_reg((uint32_t)scratch.reg, (uint32_t)src_lo.u.reg.r0, flags_safe(),
                     THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
    /* Now dst_hi = src_hi (still live), then dst_lo = scratch (=old src_lo). */
    tcc_gen_machine_assign_mop(src_hi, dst_hi, TCCIR_OP_ASSIGN);
    MachineOperand scratch_op = src_lo;
    scratch_op.u.reg.r0 = scratch.reg;
    tcc_gen_machine_assign_mop(scratch_op, dst_lo, TCCIR_OP_ASSIGN);
    restore_scratch_reg(&scratch);
  }
  else if (src_hi.kind == MACH_OP_REG && !src_hi.needs_deref &&
           dst_lo.kind == MACH_OP_REG && !dst_lo.needs_deref &&
           src_hi.u.reg.r0 == dst_lo.u.reg.r0)
  {
    /* dst_lo == src_hi register: write hi first to free src_hi's slot. */
    tcc_gen_machine_assign_mop(src_hi, dst_hi, TCCIR_OP_ASSIGN);
    tcc_gen_machine_assign_mop(src_lo, dst_lo, TCCIR_OP_ASSIGN);
  }
  else
  {
    tcc_gen_machine_assign_mop(src_lo, dst_lo, TCCIR_OP_ASSIGN);
    tcc_gen_machine_assign_mop(src_hi, dst_hi, TCCIR_OP_ASSIGN);
  }
}

/* tcc_gen_machine_assign_mop: MachineOperand-based entry point for simple
 * 32-bit value assignment.  Called from ir/codegen.c instead of
 * tcc_gen_machine_assign_op when:
 *   - Neither dest nor src requires a 64-bit or complex register pair, AND
 *   - The function does not use a static chain.
 *
 * Handles all destination kinds: MACH_OP_REG (direct), MACH_OP_SPILL
 * (via mach_get_dest_reg + mach_writeback_dest → tcc_machine_store_spill_slot),
 * and MACH_OP_PARAM_STACK (via mach_writeback_dest → tcc_machine_store_param_slot).
 *
 * Strategy: load src directly into dest_reg; use mach_ensure_in_reg only
 * as a fallback for unhandled source kinds.
 */
ST_FUNC void tcc_gen_machine_assign_mop(MachineOperand src, MachineOperand dest, TccIrOp op)
{
  (void)op;

  /* 64-bit pair assignment: handle each 32-bit half independently.
   * mach_make_lo/hi_half splits MACH_OP_REG (r0:r1), MACH_OP_SPILL (offset,
   * offset+4) and MACH_OP_IMM into separate 32-bit MachineOperands.
   * We then recursively assign each half (is_64bit=false prevents recursion).
   *
   * Special care: when src has needs_deref=true, the operand is a POINTER
   * to a 64-bit value. The address is in one register (or spill slot);
   * splitting registers via mach_make_hi_half would create a bogus base
   * address. Instead, load both halves from [base+0] and [base+4].
   *
   * Exception: PARAM_STACK with needs_deref means the 64-bit value IS
   * directly at [fp+offset], not a pointer to follow. Clear needs_deref
   * so it falls through to the normal lo/hi split path. */
  if (src.needs_deref && src.is_64bit && src.kind == MACH_OP_PARAM_STACK)
    src.needs_deref = false;

  if (dest.is_64bit)
  {
    MachineOperand dst_lo = mach_make_lo_half(&dest);
    MachineOperand dst_hi = mach_make_hi_half(&dest);
    dst_lo.btype = IROP_BTYPE_INT32;
    dst_hi.btype = IROP_BTYPE_INT32;

    if (src.needs_deref && src.is_64bit)
    {
      /* Source is a 64-bit lvalue: a pointer to a 64-bit value (e.g. R0
       * holding address of an unsigned long long). Load both 32-bit halves
       * from [base+0] and [base+4] using the same base address register. */
      MachineCodegenContext mctx = {0};

      /* Strip deref to get the raw address into a register. */
      MachineOperand addr = src;
      addr.needs_deref = false;
      addr.is_64bit = false;
      addr.btype = IROP_BTYPE_INT32;
      int base_reg = mach_ensure_in_reg(&mctx, &addr, 0);
      uint32_t excl = (1u << (uint32_t)base_reg);

      /* Determine destination registers for lo and hi halves. */
      int lo_reg = mach_get_dest_reg(&mctx, &dst_lo, excl);
      if (thumb_is_hw_reg(lo_reg))
        excl |= (1u << (uint32_t)lo_reg);
      int hi_reg = mach_get_dest_reg(&mctx, &dst_hi, excl);

      /* Load [base+0] → lo, [base+4] → hi (32-bit loads). */
      load_from_base(lo_reg, PREG_REG_NONE, IROP_BTYPE_INT32, 0, 0, 0, (uint32_t)base_reg);
      load_from_base(hi_reg, PREG_REG_NONE, IROP_BTYPE_INT32, 0, 4, 0, (uint32_t)base_reg);

      mach_writeback_dest(&dst_lo, lo_reg);
      mach_writeback_dest(&dst_hi, hi_reg);
      mach_release_all(&mctx);
      return;
    }

    if (src.is_64bit)
    {
      MachineOperand src_lo = mach_make_lo_half(&src);
      MachineOperand src_hi = mach_make_hi_half(&src);
      src_lo.btype = IROP_BTYPE_INT32;
      src_hi.btype = IROP_BTYPE_INT32;
      tcc_gen_machine_assign_mop(src_lo, dst_lo, op);
      tcc_gen_machine_assign_mop(src_hi, dst_hi, op);
    }
    else
    {
      /* 32-bit source into 64-bit dest: assign lo half, zero the high half. */
      MachineOperand zero = {0};
      zero.kind = MACH_OP_IMM;
      zero.u.imm.val = 0;
      zero.btype = IROP_BTYPE_INT32;
      tcc_gen_machine_assign_mop(src, dst_lo, op);
      tcc_gen_machine_assign_mop(zero, dst_hi, op);
    }
    return;
  }

  if (src.is_64bit && !dest.is_64bit)
  {
    /* Truncation: extract and assign only the low half of the 64-bit source. */
    MachineOperand src_lo = mach_make_lo_half(&src);
    src_lo.btype = IROP_BTYPE_INT32;
    tcc_gen_machine_assign_mop(src_lo, dest, op);
    return;
  }

  MachineCodegenContext mctx = {0};

  /* --- Fast path: source is already in a register (no dereference) ---
   * Write it directly to the destination via mach_writeback_dest without
   * allocating any scratch.  This covers REG→REG (MOV or NOP) and
   * REG→SPILL/PARAM_STACK (direct store from src register). */
  if (src.kind == MACH_OP_REG && !src.needs_deref)
  {
    mach_writeback_dest(&dest, src.u.reg.r0);
    return;
  }

  /* --- Determine destination register ---
   * For REG destinations, reuse the pre-allocated register (0 scratch).
   * For SPILL/PARAM_STACK/REG(deref) destinations, allocate a scratch. */
  int dest_reg;
  bool need_writeback;
  if (dest.kind == MACH_OP_REG && !dest.needs_deref && dest.u.reg.r0 != (int)PREG_REG_NONE)
  {
    dest_reg = dest.u.reg.r0;
    need_writeback = false;
  }
  else
  {
    dest_reg = mach_get_dest_reg(&mctx, &dest, 0);
    need_writeback = true;
  }

  /* --- Load source value directly into dest_reg --- */
  switch (src.kind)
  {
  case MACH_OP_REG:
    /* Only the needs_deref case reaches here (non-deref handled above).
     * Load from [src_reg] directly into dest_reg. */
    load_from_base(dest_reg, PREG_REG_NONE, src.btype, (int)src.is_unsigned, 0, 0, (uint32_t)src.u.reg.r0);
    break;

  case MACH_OP_IMM:
    tcc_machine_load_constant(dest_reg, PREG_REG_NONE, src.u.imm.val, 0, NULL);
    break;

  case MACH_OP_SPILL:
    tcc_machine_load_spill_slot(dest_reg, src.u.spill.offset);
    if (src.needs_deref)
    {
      /* Double indirection: dest_reg now holds a pointer; dereference it. */
      load_from_base(dest_reg, PREG_REG_NONE, src.btype, (int)src.is_unsigned, 0, 0, (uint32_t)dest_reg);
    }
    break;

  case MACH_OP_SYMBOL:
  {
    Sym *sym = src.u.sym.sym ? validate_sym_for_reloc(src.u.sym.sym) : NULL;
    if (!src.needs_deref)
    {
      tcc_machine_load_constant(dest_reg, PREG_REG_NONE, src.u.sym.addend, 0, sym);
    }
    else
    {
      /* Load symbol address into dest_reg, then dereference through it. */
      tcc_machine_load_constant(dest_reg, PREG_REG_NONE, 0, 0, sym);
      const int32_t addend = src.u.sym.addend;
      load_from_base(dest_reg, PREG_REG_NONE, src.btype, (int)src.is_unsigned,
                     addend < 0 ? (int)(-addend) : (int)addend, addend < 0 ? 1 : 0, (uint32_t)dest_reg);
    }
    break;
  }

  case MACH_OP_FRAME_ADDR:
    tcc_machine_addr_of_stack_slot(dest_reg, src.u.frame.offset, 0);
    break;

  case MACH_OP_PARAM_STACK:
  {
    const int adjusted = src.u.param.offset + offset_to_args;
    const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
    const int sign = (adjusted < 0);
    const int abs_off = sign ? -adjusted : adjusted;
    load_from_base(dest_reg, PREG_REG_NONE, src.btype, (int)src.is_unsigned, abs_off, sign, (uint32_t)base_reg);
    break;
  }

  default:
  {
    /* Fallback: generic mach_ensure_in_reg + MOV. */
    uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;
    int src_reg = mach_ensure_in_reg(&mctx, &src, excl);
    if (src_reg != dest_reg)
      ot_check_mov_reg((uint32_t)dest_reg, (uint32_t)src_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                       ENFORCE_ENCODING_NONE, false);
    break;
  }
  }

  if (need_writeback)
    mach_writeback_dest(&dest, dest_reg);

  mach_release_all(&mctx);
}

/* tcc_gen_machine_setif_mop: MachineOperand-based entry point for SETIF.
 * src must be MACH_OP_IMM carrying the raw condition code in u.imm.val.
 *
 * 32-bit dest:
 *   ITE <cond>
 *   MOV dest, #1   (T: cond met)
 *   MOV dest, #0   (E: cond not met)
 *
 * 64-bit dest pair (e.g. long long result = (x > y)):
 *   The boolean result 0 or 1 fits in 32 bits, so hi word is always 0.
 *   ITE <cond>
 *   MOV dest_lo, #1
 *   MOV dest_lo, #0
 *   MOV dest_hi, #0   (unconditional, outside IT block — hi is always 0)
 *
 * Inner MOVs use NOT_IMPORTANT for flags: SETIF is the consumer of the CMP
 * flags; once the ITE captures the condition, no subsequent code in this
 * lowering depends on CMP's flag state, so the 16-bit T1 encoding (which
 * implicitly sets flags) is safe.  This shrinks each conditional MOV from
 * 4 bytes (mov.w) to 2 bytes (movs).
 */
ST_FUNC void tcc_gen_machine_setif_mop(MachineOperand src, MachineOperand dest, TccIrOp op)
{
  (void)op;
  MachineCodegenContext mctx = {0};

  const int cond = mapcc((int)src.u.imm.val);
  /* ITE mask: 2nd instruction has opposite condition.
   * mask[3] = 1 if it should be the 'else' bit (opposite of cond[0]).
   * For the T-then-E pattern, mask = ((!cond[0]) << 3) | 0x4. */
  const uint16_t ite_mask = (uint16_t)(((cond ^ 1) & 1) << 3) | 0x4u;

  if (dest.is_64bit)
  {
    /* Split 64-bit destination into two independent 32-bit halves. */
    MachineOperand dst_lo = mach_make_lo_half(&dest);
    MachineOperand dst_hi = mach_make_hi_half(&dest);
    dst_lo.btype = IROP_BTYPE_INT32;
    dst_hi.btype = IROP_BTYPE_INT32;

    int lo_reg = mach_get_dest_reg(&mctx, &dst_lo, 0);
    uint32_t excl = thumb_is_hw_reg(lo_reg) ? (1u << (uint32_t)lo_reg) : 0u;
    int hi_reg = mach_get_dest_reg(&mctx, &dst_hi, excl);

    /* Emit ITE sequence for lo word. */
    th_literal_pool_reserve_upcoming_bytes(6);
    ot_check(th_it(cond, ite_mask)); /* ITE <cond> — two conditioned instructions */
    ot_check(th_mov_imm(lo_reg, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    ot_check(th_mov_imm(lo_reg, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    /* Hi word is always 0 — boolean result never exceeds 1 (i.e. fits in 32-bit lo). */
    ot_check(th_mov_imm(hi_reg, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

    mach_writeback_dest(&dst_lo, lo_reg);
    mach_writeback_dest(&dst_hi, hi_reg);
  }
  else
  {
    int dest_reg = mach_get_dest_reg(&mctx, &dest, 0);

    th_literal_pool_reserve_upcoming_bytes(6);
    ot_check(th_it(cond, ite_mask)); /* ITE <cond> — two conditioned instructions */
    ot_check(th_mov_imm(dest_reg, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    ot_check(th_mov_imm(dest_reg, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

    mach_writeback_dest(&dest, dest_reg);
  }

  mach_release_all(&mctx);
}

/* tcc_gen_machine_bool_mop: MachineOperand-based entry point for
 * BOOL_OR / BOOL_AND.  Called from ir/codegen.c for simple 32-bit
 * non-complex boolean operations.
 *
 * BOOL_OR:   ORRS dest, src1, src2   (sets Z flag)
 *            MOV  dest, #0           (flag-preserving)
 *            IT   NE
 *            MOV  dest, #1
 *
 * BOOL_AND:  CMP  src1, #0
 *            IT   NE
 *            CMP  src2, #0           (only if src1 != 0)
 *            MOV  dest, #0           (flag-preserving)
 *            IT   NE
 *            MOV  dest, #1
 */
ST_FUNC void tcc_gen_machine_bool_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op)
{
  MachineCodegenContext mctx = {0};

  /* 64-bit operands: reduce each to a 32-bit "is non-zero" value by OR-ing
   * its low and high halves, then apply the standard 32-bit BOOL logic. */
  if (src1.is_64bit || src2.is_64bit)
  {
    uint32_t excl = 0;
    int r1, r2;

    if (src1.is_64bit)
    {
      MachineOperand lo1 = mach_make_lo_half(&src1);
      lo1.btype = IROP_BTYPE_INT32;
      MachineOperand hi1 = mach_make_hi_half(&src1);
      hi1.btype = IROP_BTYPE_INT32;
      r1 = mach_ensure_in_reg(&mctx, &lo1, excl);
      excl |= thumb_is_hw_reg(r1) ? (1u << (uint32_t)r1) : 0;
      int hi1_reg = mach_ensure_in_reg(&mctx, &hi1, excl);
      excl |= thumb_is_hw_reg(hi1_reg) ? (1u << (uint32_t)hi1_reg) : 0;
      /* r1 = lo1 | hi1 — is src1 non-zero? */
      ot_check(th_orr_reg(r1, r1, hi1_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    else
    {
      r1 = mach_ensure_in_reg(&mctx, &src1, excl);
      excl |= thumb_is_hw_reg(r1) ? (1u << (uint32_t)r1) : 0;
    }

    if (src2.is_64bit)
    {
      MachineOperand lo2 = mach_make_lo_half(&src2);
      lo2.btype = IROP_BTYPE_INT32;
      MachineOperand hi2 = mach_make_hi_half(&src2);
      hi2.btype = IROP_BTYPE_INT32;
      r2 = mach_ensure_in_reg(&mctx, &lo2, excl);
      excl |= thumb_is_hw_reg(r2) ? (1u << (uint32_t)r2) : 0;
      int hi2_reg = mach_ensure_in_reg(&mctx, &hi2, excl);
      excl |= thumb_is_hw_reg(hi2_reg) ? (1u << (uint32_t)hi2_reg) : 0;
      /* r2 = lo2 | hi2 — is src2 non-zero? */
      ot_check(th_orr_reg(r2, r2, hi2_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    else
    {
      r2 = mach_ensure_in_reg(&mctx, &src2, excl);
      excl |= thumb_is_hw_reg(r2) ? (1u << (uint32_t)r2) : 0;
    }

    int dest_reg = mach_get_dest_reg(&mctx, &dest, excl);

    if (op == TCCIR_OP_BOOL_OR)
    {
      ot_check(th_orr_reg(dest_reg, r1, r2, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      ot_check(th_mov_imm(dest_reg, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE));
      th_literal_pool_reserve_upcoming_bytes(6);
      ot_check(th_it(0x1, 0x8)); /* IT NE */
      ot_check(th_mov_imm(dest_reg, 1, flags_safe(), ENFORCE_ENCODING_NONE));
    }
    else /* TCCIR_OP_BOOL_AND */
    {
      ot_check(th_cmp_imm(r1, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
      th_literal_pool_reserve_upcoming_bytes(6);
      ot_check(th_it(0x1, 0x8));                                                  /* IT NE */
      ot_check(th_cmp_imm(r2, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE)); /* CMPne r2, #0 */
      ot_check(th_mov_imm(dest_reg, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE));
      th_literal_pool_reserve_upcoming_bytes(6);
      ot_check(th_it(0x1, 0x8)); /* IT NE */
      ot_check(th_mov_imm(dest_reg, 1, flags_safe(), ENFORCE_ENCODING_NONE));
    }

    mach_writeback_dest(&dest, dest_reg);
    mach_release_all(&mctx);
    return;
  }

  int dest_reg = mach_get_dest_reg(&mctx, &dest, 0);
  uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;

  int src1_reg = mach_ensure_in_reg(&mctx, &src1, excl);
  if (thumb_is_hw_reg(src1_reg))
    excl |= (1u << (uint32_t)src1_reg);

  int src2_reg = mach_ensure_in_reg(&mctx, &src2, excl);

  if (op == TCCIR_OP_BOOL_OR)
  {
    ot_check(th_orr_reg(dest_reg, src1_reg, src2_reg, FLAGS_BEHAVIOUR_SET, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    ot_check(th_mov_imm(dest_reg, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE));
    th_literal_pool_reserve_upcoming_bytes(6);
    ot_check(th_it(0x1, 0x8)); /* IT NE */
    ot_check(th_mov_imm(dest_reg, 1, flags_safe(), ENFORCE_ENCODING_NONE));
  }
  else /* TCCIR_OP_BOOL_AND */
  {
    ot_check(th_cmp_imm(src1_reg, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
    th_literal_pool_reserve_upcoming_bytes(6);
    ot_check(th_it(0x1, 0x8));                                                        /* IT NE */
    ot_check(th_cmp_imm(src2_reg, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE)); /* CMPne src2, #0 */
    ot_check(th_mov_imm(dest_reg, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE));
    th_literal_pool_reserve_upcoming_bytes(6);
    ot_check(th_it(0x1, 0x8)); /* IT NE */
    ot_check(th_mov_imm(dest_reg, 1, flags_safe(), ENFORCE_ENCODING_NONE));
  }

  mach_writeback_dest(&dest, dest_reg);
  mach_release_all(&mctx);
}

/* tcc_gen_machine_load_mop: MachineOperand-based entry point for TCCIR_OP_LOAD.
 *
 * dest can be MACH_OP_REG, MACH_OP_SPILL, or MACH_OP_PARAM_STACK.
 * For spilled destinations, a scratch register is allocated and the result
 * is written back to the spill slot after the load completes.
 * 64-bit dest is supported: for MACH_OP_REG dest.u.reg.r1 holds the hi
 * register; for spilled dests, a second scratch is allocated for hi-half.
 *
 * src encodes the memory address:
 *   MACH_OP_REG + needs_deref=true  → LDR dest, [src_reg]
 *   MACH_OP_SPILL                   → LDR dest, [FP + fp_adjust(offset)]
 *   MACH_OP_SPILL + needs_deref=true → LLOCAL: LDR ptr,[FP+off]; LDR dest,[ptr]
 *   MACH_OP_PARAM_STACK             → LDR dest, [FP + param_off + offset_to_args]
 *   MACH_OP_SYMBOL                  → LDR_literal addr; LDR dest, [addr]
 *   MACH_OP_IMM                     → tcc_machine_load_constant (constant load)
 */
ST_FUNC void tcc_gen_machine_load_mop(MachineOperand src, MachineOperand dest, TccIrOp op)
{
  MachineCodegenContext ctx = {0};
  (void)op;

  /* Determine dest register — allocates scratch if dest is SPILL/PARAM_STACK. */
  const bool dest_is_simple_reg =
      (dest.kind == MACH_OP_REG && !dest.needs_deref && dest.u.reg.r0 != (int)PREG_REG_NONE);
  const int dest_reg = mach_get_dest_reg(&ctx, &dest, 0);

  /* For 64-bit pairs: get hi-half dest register. */
  int dest_r1 = PREG_REG_NONE;
  MachineOperand dest_hi_mop = {0};
  if (dest.is_64bit)
  {
    if (dest_is_simple_reg)
    {
      dest_r1 = dest.u.reg.r1;
    }
    else
    {
      dest_hi_mop = mach_make_hi_half(&dest);
      dest_r1 = mach_get_dest_reg(&ctx, &dest_hi_mop, (1u << (uint32_t)dest_reg));
    }
  }

  const int btype = src.btype;
  const int is_unsigned = (int)src.is_unsigned;

  switch (src.kind)
  {
  case MACH_OP_REG:
    if (src.needs_deref)
    {
      /* Register-indirect: LDR dest, [src_reg] */
      load_from_base(dest_reg, dest_r1, btype, is_unsigned, 0, 0, (uint32_t)src.u.reg.r0);
    }
    else
    {
      /* Direct register-to-register (treat as MOV — should be ASSIGN, not LOAD) */
      if (dest_reg != src.u.reg.r0)
        ot_check_mov_reg((uint32_t)dest_reg, (uint32_t)src.u.reg.r0, flags_safe(), THUMB_SHIFT_DEFAULT,
                         ENFORCE_ENCODING_NONE, false);
      /* Narrow sub-word parameter values: when a parameter is declared as
       * char/short but arrives in a full 32-bit register (AAPCS default
       * argument promotion), the upper bits may contain garbage.  Emit
       * UXTB/SXTB/UXTH/SXTH to truncate to the declared type width.  */
      if (btype == IROP_BTYPE_INT8)
      {
        if (is_unsigned)
          ot_check(th_uxtb((uint32_t)dest_reg, (uint32_t)dest_reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        else
          ot_check(th_sxtb((uint32_t)dest_reg, (uint32_t)dest_reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      }
      else if (btype == IROP_BTYPE_INT16)
      {
        if (is_unsigned)
          ot_check(th_uxth((uint32_t)dest_reg, (uint32_t)dest_reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        else
          ot_check(th_sxth((uint32_t)dest_reg, (uint32_t)dest_reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      }
      /* 64-bit pair: also copy the hi-half register */
      if (dest_r1 != PREG_REG_NONE && src.u.reg.r1 >= 0 && dest_r1 != src.u.reg.r1)
        ot_check_mov_reg((uint32_t)dest_r1, (uint32_t)src.u.reg.r1, flags_safe(), THUMB_SHIFT_DEFAULT,
                         ENFORCE_ENCODING_NONE, false);
    }
    break;

  case MACH_OP_SPILL:
  {
    const int adj = fp_adjust_local_offset(src.u.spill.offset, 0);
    const int sign = (adj < 0), abs_off = sign ? -adj : adj;
    const uint32_t base = (uint32_t)(tcc_state->need_frame_pointer ? R_FP : R_SP);
    if (!src.needs_deref)
    {
      /* Load value directly from spill/local slot */
      load_from_base(dest_reg, dest_r1, btype, is_unsigned, abs_off, sign, base);
    }
    else
    {
      /* LLOCAL: spill slot holds a pointer; load ptr, then dereference */
      int ptr_r = mach_alloc_scratch(&ctx, ((uint32_t)1u << (uint32_t)dest_reg) | ((uint32_t)1u << base));
      if (!load_word_from_base(ptr_r, (int)base, abs_off, sign))
      {
        ScratchRegAlloc rr = th_offset_to_reg_ex(abs_off, sign, ((uint32_t)1u << ptr_r) | ((uint32_t)1u << base));
        ot_check(th_ldr_reg((uint32_t)ptr_r, base, (uint32_t)rr.reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&rr);
      }
      load_from_base(dest_reg, dest_r1, btype, is_unsigned, 0, 0, (uint32_t)ptr_r);
    }
    break;
  }

  case MACH_OP_PARAM_STACK:
  {
    const int adj = src.u.param.offset + offset_to_args;
    const int sign = (adj < 0), abs_off = sign ? -adj : adj;
    const uint32_t base = (uint32_t)(tcc_state->need_frame_pointer ? R_FP : R_SP);
    load_from_base(dest_reg, dest_r1, btype, is_unsigned, abs_off, sign, base);
    break;
  }

  case MACH_OP_SYMBOL:
  {
    Sym *sym = src.u.sym.sym ? validate_sym_for_reloc(src.u.sym.sym) : NULL;
    const int32_t addend = src.u.sym.addend;
    if (!src.needs_deref)
    {
      /* Load symbol address (+ addend) into dest — no dereference.
       * Load symbol address (+ addend) — no dereference. */
      tcc_machine_load_constant(dest_reg, dest_r1, (int64_t)addend, (int)dest.is_64bit, sym);
      break;
    }
    /* needs_deref: load symbol address into scratch, then dereference. */
    int addr_r = mach_alloc_scratch(&ctx, (uint32_t)1u << (uint32_t)dest_reg);
    tcc_machine_load_constant(addr_r, PREG_REG_NONE, 0, 0, sym);
    /* For a 64-bit deref, try LDRD when we can prove the symbol's address
     * at `addend` is 4-byte aligned.  Otherwise fall back to the pair of
     * 32-bit loads via load_from_base. */
    const int sym_sign = (addend < 0), sym_abs = sym_sign ? (int)(-addend) : (int)addend;
    if (dest.is_64bit && dest_r1 != PREG_REG_NONE && sym_is_4_byte_aligned_for_64bit(sym, addend) &&
        try_ldrd_pair(dest_reg, dest_r1, addr_r, sym_abs, sym_sign))
    {
      break;
    }
    load_from_base(dest_reg, dest_r1, btype, is_unsigned, sym_abs, sym_sign, (uint32_t)addr_r);
    break;
  }

  case MACH_OP_IMM:
    /* Treat as constant load (e.g. loading from address 0 — rare but handle gracefully) */
    tcc_machine_load_constant(dest_reg, dest_r1, src.u.imm.val, (int)dest.is_64bit, NULL);
    break;

  case MACH_OP_FRAME_ADDR:
  {
    if (!src.needs_deref)
    {
      /* Load the frame-slot address itself (LEA semantics). */
      tcc_machine_addr_of_stack_slot(dest_reg, src.u.frame.offset, 0);
    }
    else
    {
      /* Frame address is a pointer to data — compute addr, then dereference. */
      int addr_r = mach_alloc_scratch(&ctx, (uint32_t)1u << (uint32_t)dest_reg);
      tcc_machine_addr_of_stack_slot(addr_r, src.u.frame.offset, 0);
      load_from_base(dest_reg, dest_r1, btype, is_unsigned, 0, 0, (uint32_t)addr_r);
    }
    break;
  }

  case MACH_OP_CHAIN_REL:
  {
    /* Captured variable: load from parent frame via static chain. */
    ScratchRegAlloc chain_scratch = {0};
    int chain_used = 0;
    int base = resolve_chain_base(tcc_state->ir, src.u.chain.chain_index, (1u << (uint32_t)dest_reg), &chain_scratch,
                                  &chain_used);
    int32_t off = src.u.chain.offset;
    int sign = (off < 0), abs_off = sign ? (int)(-off) : (int)off;
    load_from_base(dest_reg, dest_r1, btype, is_unsigned, abs_off, sign, (uint32_t)base);
    if (chain_used)
      restore_scratch_reg(&chain_scratch);
    break;
  }

  default:
    tcc_error("compiler_error: load_mop: unhandled src kind %d", (int)src.kind);
  }

  /* Write back result to spill/param slot if dest was not a plain register. */
  if (!dest_is_simple_reg)
  {
    if (dest.is_64bit)
    {
      MachineOperand dest_lo_mop = mach_make_lo_half(&dest);
      mach_writeback_dest(&dest_lo_mop, dest_reg);
      mach_writeback_dest(&dest_hi_mop, dest_r1);
    }
    else
    {
      mach_writeback_dest(&dest, dest_reg);
    }
  }

  mach_release_all(&ctx);
}

/* Forward declarations for complex splitting helpers (defined in complex MOP section below). */
static MachineOperand mach_make_complex_real(const MachineOperand *op);
static MachineOperand mach_make_complex_imag(const MachineOperand *op);

/* tcc_gen_machine_store_mop: MachineOperand-based entry point for TCCIR_OP_STORE.
 *
 * dest encodes the destination address (memory location to write to).
 * src encodes the value to store.
 * Store width is determined by dest.btype.
 *
 * dest kinds handled:
 *   MACH_OP_REG + needs_deref=true  → STR src, [dest_reg]
 *   MACH_OP_REG (no deref)          → MOV dest_reg, src (reg-to-reg)
 *   MACH_OP_SPILL                   → STR src, [FP + fp_adjust(offset)]
 *   MACH_OP_PARAM_STACK             → STR src, [FP + param_off + offset_to_args]
 *   MACH_OP_SYMBOL                  → load addr, STR src, [addr + addend]
 *
 * 64-bit src: emits two 32-bit stores at [dest+0] (lo) and [dest+4] (hi)
 * for all dest kinds above, plus MACH_OP_IMM and MACH_OP_FRAME_ADDR.
 */
ST_FUNC void tcc_gen_machine_store_mop(MachineOperand dest, MachineOperand src, TccIrOp op)
{
  MachineCodegenContext ctx = {0};
  (void)op;

  /* 128-bit complex double store: emit four 32-bit stores for real lo/hi + imag lo/hi.
   * Complex double values are 16 bytes: real (8 bytes) at base, imag (8 bytes) at base+8.
   * The source is always spilled (force-spilled by the register allocator). */
  if (src.is_64bit && src.is_complex && src.btype == IROP_BTYPE_FLOAT64)
  {
    /* Split into four 32-bit words using complex then lo/hi splitting. */
    MachineOperand real_part = mach_make_complex_real(&src);
    MachineOperand imag_part = mach_make_complex_imag(&src);
    MachineOperand w0 = mach_make_lo_half(&real_part);
    w0.btype = IROP_BTYPE_INT32;
    MachineOperand w1 = mach_make_hi_half(&real_part);
    w1.btype = IROP_BTYPE_INT32;
    MachineOperand w2 = mach_make_lo_half(&imag_part);
    w2.btype = IROP_BTYPE_INT32;
    MachineOperand w3 = mach_make_hi_half(&imag_part);
    w3.btype = IROP_BTYPE_INT32;

    /* Load all 4 words into registers. */
    const int r0 = mach_ensure_in_reg(&ctx, &w0, 0);
    uint32_t excl = (1u << (uint32_t)r0);
    const int r1 = mach_ensure_in_reg(&ctx, &w1, excl);
    excl |= (1u << (uint32_t)r1);
    const int r2 = mach_ensure_in_reg(&ctx, &w2, excl);
    excl |= (1u << (uint32_t)r2);
    const int r3 = mach_ensure_in_reg(&ctx, &w3, excl);
    excl |= (1u << (uint32_t)r3);

    /* Store through dest: 4 × 32-bit stores at [dest+0], [dest+4], [dest+8], [dest+12]. */
    if (dest.kind == MACH_OP_REG && dest.needs_deref)
    {
      const uint32_t base = (uint32_t)dest.u.reg.r0;
      th_store32_imm_or_reg_ex(r0, base, 0, 0, excl | (1u << base));
      th_store32_imm_or_reg_ex(r1, base, 4, 0, excl | (1u << base));
      th_store32_imm_or_reg_ex(r2, base, 8, 0, excl | (1u << base));
      th_store32_imm_or_reg_ex(r3, base, 12, 0, excl | (1u << base));
    }
    else if (dest.kind == MACH_OP_SPILL)
    {
      const int adj = fp_adjust_local_offset(dest.u.spill.offset, 0);
      const uint32_t base = (uint32_t)(tcc_state->need_frame_pointer ? R_FP : R_SP);
      th_store32_imm_or_reg_ex(r0, base, adj < 0 ? -adj : adj, adj < 0 ? 1 : 0, excl | (1u << base));
      int a1 = adj + 4;
      th_store32_imm_or_reg_ex(r1, base, a1 < 0 ? -a1 : a1, a1 < 0 ? 1 : 0, excl | (1u << base));
      int a2 = adj + 8;
      th_store32_imm_or_reg_ex(r2, base, a2 < 0 ? -a2 : a2, a2 < 0 ? 1 : 0, excl | (1u << base));
      int a3 = adj + 12;
      th_store32_imm_or_reg_ex(r3, base, a3 < 0 ? -a3 : a3, a3 < 0 ? 1 : 0, excl | (1u << base));
    }
    else
    {
      tcc_error("compiler_error: store_mop: unhandled dest kind %d for complex double store", (int)dest.kind);
    }
    mach_release_all(&ctx);
    return;
  }

  /* 64-bit store: emit two 32-bit stores for lo and hi halves */
  if (src.is_64bit)
  {
    MachineOperand src_lo = mach_make_lo_half(&src);
    src_lo.btype = IROP_BTYPE_INT32;
    MachineOperand src_hi = mach_make_hi_half(&src);
    src_hi.btype = IROP_BTYPE_INT32;

    uint32_t dest_excl = 0;
    if (dest.kind == MACH_OP_REG && dest.needs_deref &&
        dest.u.reg.r0 >= 0 && dest.u.reg.r0 < 16)
      dest_excl = (1u << (uint32_t)dest.u.reg.r0);

    const int lo_reg = mach_ensure_in_reg(&ctx, &src_lo, dest_excl);
    uint32_t excl = dest_excl | (thumb_is_hw_reg(lo_reg) ? (1u << (uint32_t)lo_reg) : 0u);
    const int hi_reg = mach_ensure_in_reg(&ctx, &src_hi, excl);
    excl |= thumb_is_hw_reg(hi_reg) ? (1u << (uint32_t)hi_reg) : 0u;

    switch (dest.kind)
    {
    case MACH_OP_REG:
      if (dest.needs_deref)
      {
        /* 64-bit pointer-store through a register-held address.  Do NOT use
         * STRD here: ARMv7-M/v8-M requires 4-byte alignment for STRD and
         * faults otherwise, but the pointer may target packed-struct memory
         * that is only 1- or 2-byte aligned.  Plain STR tolerates unaligned
         * (UNALIGN_TRP=0 default) so two 32-bit stores stay safe. */
        const uint32_t base = (uint32_t)dest.u.reg.r0;
        th_store32_imm_or_reg_ex(lo_reg, base, 0, 0, excl | (1u << base));
        th_store32_imm_or_reg_ex(hi_reg, base, 4, 0, excl | (1u << base));
      }
      else
      {
        /* Reg-pair dst: emit hi first unless lo_reg == dest.r1 (safe-ordering) */
        const int dreg_lo = dest.u.reg.r0;
        const int dreg_hi = dest.u.reg.r1;
        if (lo_reg == dreg_hi)
        {
          if (dreg_lo != lo_reg && dreg_lo != (int)PREG_REG_NONE)
            ot_check_mov_reg((uint32_t)dreg_lo, (uint32_t)lo_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                             ENFORCE_ENCODING_NONE, false);
          if (dreg_hi != hi_reg && dreg_hi != (int)PREG_REG_NONE)
            ot_check_mov_reg((uint32_t)dreg_hi, (uint32_t)hi_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                             ENFORCE_ENCODING_NONE, false);
        }
        else
        {
          if (dreg_hi != hi_reg && dreg_hi != (int)PREG_REG_NONE)
            ot_check_mov_reg((uint32_t)dreg_hi, (uint32_t)hi_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                             ENFORCE_ENCODING_NONE, false);
          if (dreg_lo != lo_reg && dreg_lo != (int)PREG_REG_NONE)
            ot_check_mov_reg((uint32_t)dreg_lo, (uint32_t)lo_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                             ENFORCE_ENCODING_NONE, false);
        }
      }
      break;

    case MACH_OP_SPILL:
    {
      const int adj = fp_adjust_local_offset(dest.u.spill.offset, 0);
      const uint32_t base = (uint32_t)(tcc_state->need_frame_pointer ? R_FP : R_SP);
      if (dest.needs_deref)
      {
        /* LLOCAL: spill slot holds a pointer; load ptr, then store through it */
        int ptr_r = mach_alloc_scratch(&ctx, excl | (1u << base));
        if (!load_word_from_base(ptr_r, (int)base, adj < 0 ? -adj : adj, adj < 0 ? 1 : 0))
        {
          ScratchRegAlloc rr =
              th_offset_to_reg_ex(adj < 0 ? -adj : adj, adj < 0 ? 1 : 0, (uint32_t)(1u << ptr_r) | (1u << base));
          ot_check(th_ldr_reg((uint32_t)ptr_r, base, (uint32_t)rr.reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          restore_scratch_reg(&rr);
        }
        /* Pointer-through store from an LLOCAL spill slot: the target
         * address is arbitrary (may be unaligned packed-struct memory), so
         * skip STRD. */
        th_store32_imm_or_reg_ex(lo_reg, (uint32_t)ptr_r, 0, 0, excl | (1u << (uint32_t)ptr_r));
        th_store32_imm_or_reg_ex(hi_reg, (uint32_t)ptr_r, 4, 0, excl | (1u << (uint32_t)ptr_r));
      }
      else
      {
        const int adj_hi = adj + 4;
        const int sign = (adj < 0), abs_off = sign ? -adj : adj;
        if (!try_strd_pair(lo_reg, hi_reg, (int)base, abs_off, sign))
        {
          th_store32_imm_or_reg_ex(lo_reg, base, abs_off, sign, excl | (1u << base));
          th_store32_imm_or_reg_ex(hi_reg, base, adj_hi < 0 ? -adj_hi : adj_hi, adj_hi < 0 ? 1 : 0,
                                   excl | (1u << base));
        }
      }
      break;
    }

    case MACH_OP_PARAM_STACK:
    {
      const int adj = dest.u.param.offset + offset_to_args;
      const int adj_hi = adj + 4;
      const uint32_t base = (uint32_t)(tcc_state->need_frame_pointer ? R_FP : R_SP);
      const int sign = (adj < 0), abs_off = sign ? -adj : adj;
      if (!try_strd_pair(lo_reg, hi_reg, (int)base, abs_off, sign))
      {
        th_store32_imm_or_reg_ex(lo_reg, base, abs_off, sign, excl | (1u << base));
        th_store32_imm_or_reg_ex(hi_reg, base, adj_hi < 0 ? -adj_hi : adj_hi, adj_hi < 0 ? 1 : 0, excl | (1u << base));
      }
      break;
    }

    case MACH_OP_SYMBOL:
    {
      /* Global symbol store.  STRD needs 4-byte alignment; allow it only when
       * the symbol's declared type guarantees natural alignment >= 4 (regular
       * scalar globals) or the symbol was explicitly aligned.  Packed structs
       * and struct-typed globals stay on the STR-pair path. */
      Sym *sym = dest.u.sym.sym ? validate_sym_for_reloc(dest.u.sym.sym) : NULL;
      int addr_r = mach_alloc_scratch(&ctx, excl);
      tcc_machine_load_constant(addr_r, PREG_REG_NONE, 0, 0, sym);
      const int32_t addend = dest.u.sym.addend;
      const int32_t addend_hi = addend + 4;
      const int sign = (addend < 0), abs_off = sign ? (int)(-addend) : (int)addend;
      if (sym_is_4_byte_aligned_for_64bit(sym, addend) && try_strd_pair(lo_reg, hi_reg, addr_r, abs_off, sign))
      {
        break;
      }
      th_store32_imm_or_reg_ex(lo_reg, (uint32_t)addr_r, abs_off, sign, excl | (1u << addr_r));
      th_store32_imm_or_reg_ex(hi_reg, (uint32_t)addr_r, addend_hi < 0 ? (int)(-addend_hi) : (int)addend_hi,
                               addend_hi < 0 ? 1 : 0, excl | (1u << addr_r));
      break;
    }

    case MACH_OP_IMM:
    {
      /* Store to a constant address — alignment unknown, skip STRD. */
      int addr_r = mach_alloc_scratch(&ctx, excl);
      tcc_machine_load_constant(addr_r, PREG_REG_NONE, dest.u.imm.val, 0, NULL);
      th_store32_imm_or_reg_ex(lo_reg, (uint32_t)addr_r, 0, 0, excl | (1u << addr_r));
      th_store32_imm_or_reg_ex(hi_reg, (uint32_t)addr_r, 4, 0, excl | (1u << addr_r));
      break;
    }

    case MACH_OP_FRAME_ADDR:
    {
      int addr_r = mach_alloc_scratch(&ctx, excl);
      tcc_machine_addr_of_stack_slot(addr_r, dest.u.frame.offset, 0 /* not param */);
      if (!try_strd_pair(lo_reg, hi_reg, addr_r, 0, 0))
      {
        th_store32_imm_or_reg_ex(lo_reg, (uint32_t)addr_r, 0, 0, excl | (1u << addr_r));
        th_store32_imm_or_reg_ex(hi_reg, (uint32_t)addr_r, 4, 0, excl | (1u << addr_r));
      }
      break;
    }

    case MACH_OP_CHAIN_REL:
    {
      /* 64-bit captured variable: store lo+hi words to parent frame. */
      ScratchRegAlloc chain_scratch = {0};
      int chain_used = 0;
      int base = resolve_chain_base(tcc_state->ir, dest.u.chain.chain_index, excl, &chain_scratch, &chain_used);
      int32_t off = dest.u.chain.offset;
      int sign = (off < 0), abs_off = sign ? (int)(-off) : (int)off;
      int32_t off_hi = off + 4;
      int sign_hi = (off_hi < 0), abs_off_hi = sign_hi ? (int)(-off_hi) : (int)off_hi;
      if (!try_strd_pair(lo_reg, hi_reg, base, abs_off, sign))
      {
        th_store32_imm_or_reg_ex(lo_reg, (uint32_t)base, abs_off, sign, excl | (1u << (uint32_t)base));
        th_store32_imm_or_reg_ex(hi_reg, (uint32_t)base, abs_off_hi, sign_hi, excl | (1u << (uint32_t)base));
      }
      if (chain_used)
        restore_scratch_reg(&chain_scratch);
      break;
    }

    default:
      tcc_error("compiler_error: store_mop: unhandled dest kind %d for 64-bit src", (int)dest.kind);
    }
    mach_release_all(&ctx);
    return;
  }

  const int btype = dest.btype; /* Store width from destination type */

  /* Fast path: plain-register dest (no deref) — load src directly into dest,
   * skipping the intermediate scratch + MOV that the generic path emits.
   * Covers IMM, SYMBOL, SPILL, FRAME_ADDR, PARAM_STACK, CHAIN_REL, and
   * REG-with-or-without-deref src kinds. */
  if (dest.kind == MACH_OP_REG && !dest.needs_deref && dest.u.reg.r0 != (int)PREG_REG_NONE)
  {
    tcc_gen_mach_load_to_reg(dest.u.reg.r0, &src);
    mach_release_all(&ctx);
    return;
  }

  /* Get source value register — may allocate a scratch if spilled/const.
   * When storing through a register-held pointer, protect the base register
   * before materializing immediates or spilled values. */
  uint32_t src_excl = 0;
  if (dest.kind == MACH_OP_REG && dest.needs_deref &&
      dest.u.reg.r0 >= 0 && dest.u.reg.r0 < 16)
    src_excl |= (1u << (uint32_t)dest.u.reg.r0);
  const int src_reg = mach_ensure_in_reg(&ctx, &src, src_excl);

  switch (dest.kind)
  {
  case MACH_OP_REG:
    if (dest.needs_deref)
    {
      /* Store through pointer: STR src, [dest_reg] */
      const uint32_t base = (uint32_t)dest.u.reg.r0;
      if (btype == IROP_BTYPE_INT8)
        th_store8_imm_or_reg(src_reg, base, 0, 0);
      else if (btype == IROP_BTYPE_INT16)
        th_store16_imm_or_reg(src_reg, base, 0, 0);
      else
        th_store32_imm_or_reg_ex(src_reg, base, 0, 0, (uint32_t)1u << (uint32_t)src_reg);
    }
    else
    {
      /* Register-to-register store (MOV) */
      const int dreg = dest.u.reg.r0;
      if (dreg != src_reg && dreg != (int)PREG_REG_NONE)
        ot_check_mov_reg((uint32_t)dreg, (uint32_t)src_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                         ENFORCE_ENCODING_NONE, false);
    }
    break;

  case MACH_OP_SPILL:
  {
    const int adj = fp_adjust_local_offset(dest.u.spill.offset, 0);
    const int sign = (adj < 0), abs_off = sign ? -adj : adj;
    const uint32_t base = (uint32_t)(tcc_state->need_frame_pointer ? R_FP : R_SP);
    if (dest.needs_deref)
    {
      /* LLOCAL: spill slot holds a pointer; load ptr, then store through it */
      int ptr_r = mach_alloc_scratch(&ctx, (uint32_t)1u << (uint32_t)src_reg | (1u << base));
      if (!load_word_from_base(ptr_r, (int)base, abs_off, sign))
      {
        ScratchRegAlloc rr =
            th_offset_to_reg_ex(abs_off, sign, (uint32_t)(1u << ptr_r) | (1u << base) | (1u << (uint32_t)src_reg));
        ot_check(th_ldr_reg((uint32_t)ptr_r, base, (uint32_t)rr.reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&rr);
      }
      if (btype == IROP_BTYPE_INT8)
        th_store8_imm_or_reg(src_reg, (uint32_t)ptr_r, 0, 0);
      else if (btype == IROP_BTYPE_INT16)
        th_store16_imm_or_reg(src_reg, (uint32_t)ptr_r, 0, 0);
      else
        th_store32_imm_or_reg_ex(src_reg, (uint32_t)ptr_r, 0, 0,
                                 (uint32_t)1u << (uint32_t)src_reg | (1u << (uint32_t)ptr_r));
    }
    else
    {
      if (btype == IROP_BTYPE_INT8)
        th_store8_imm_or_reg(src_reg, base, abs_off, sign);
      else if (btype == IROP_BTYPE_INT16)
        th_store16_imm_or_reg(src_reg, base, abs_off, sign);
      else
        th_store32_imm_or_reg_ex(src_reg, base, abs_off, sign, (uint32_t)1u << (uint32_t)src_reg);
    }
    break;
  }

  case MACH_OP_PARAM_STACK:
  {
    const int adj = dest.u.param.offset + offset_to_args;
    const int sign = (adj < 0), abs_off = sign ? -adj : adj;
    const uint32_t base = (uint32_t)(tcc_state->need_frame_pointer ? R_FP : R_SP);
    if (btype == IROP_BTYPE_INT8)
      th_store8_imm_or_reg(src_reg, base, abs_off, sign);
    else if (btype == IROP_BTYPE_INT16)
      th_store16_imm_or_reg(src_reg, base, abs_off, sign);
    else
      th_store32_imm_or_reg_ex(src_reg, base, abs_off, sign, (uint32_t)1u << (uint32_t)src_reg);
    break;
  }

  case MACH_OP_SYMBOL:
  {
    Sym *sym = dest.u.sym.sym ? validate_sym_for_reloc(dest.u.sym.sym) : NULL;
    int addr_r = mach_alloc_scratch(&ctx, (uint32_t)1u << (uint32_t)src_reg);
    tcc_machine_load_constant(addr_r, PREG_REG_NONE, 0, 0, sym);
    const int32_t addend = dest.u.sym.addend;
    const int abs_off = addend < 0 ? (int)(-addend) : (int)addend;
    const int sign = addend < 0 ? 1 : 0;
    if (btype == IROP_BTYPE_INT8)
      th_store8_imm_or_reg(src_reg, (uint32_t)addr_r, abs_off, sign);
    else if (btype == IROP_BTYPE_INT16)
      th_store16_imm_or_reg(src_reg, (uint32_t)addr_r, abs_off, sign);
    else
      th_store32_imm_or_reg_ex(src_reg, (uint32_t)addr_r, abs_off, sign, (uint32_t)1u << (uint32_t)src_reg);
    break;
  }

  case MACH_OP_IMM:
  {
    /* Store to an absolute address — e.g. *(volatile uint32_t*)0xABCD = val */
    int addr_r = mach_alloc_scratch(&ctx, (uint32_t)1u << (uint32_t)src_reg);
    tcc_machine_load_constant(addr_r, PREG_REG_NONE, dest.u.imm.val, 0, NULL);
    if (btype == IROP_BTYPE_INT8)
      th_store8_imm_or_reg(src_reg, (uint32_t)addr_r, 0, 0);
    else if (btype == IROP_BTYPE_INT16)
      th_store16_imm_or_reg(src_reg, (uint32_t)addr_r, 0, 0);
    else
      th_store32_imm_or_reg_ex(src_reg, (uint32_t)addr_r, 0, 0, (uint32_t)1u << (uint32_t)src_reg);
    break;
  }

  case MACH_OP_FRAME_ADDR:
  {
    /* Store to a frame-relative address; equivalent to MACH_OP_SPILL but via addr computation */
    int addr_r = mach_alloc_scratch(&ctx, (uint32_t)1u << (uint32_t)src_reg);
    tcc_machine_addr_of_stack_slot(addr_r, dest.u.frame.offset, 0 /* not param */);
    if (btype == IROP_BTYPE_INT8)
      th_store8_imm_or_reg(src_reg, (uint32_t)addr_r, 0, 0);
    else if (btype == IROP_BTYPE_INT16)
      th_store16_imm_or_reg(src_reg, (uint32_t)addr_r, 0, 0);
    else
      th_store32_imm_or_reg_ex(src_reg, (uint32_t)addr_r, 0, 0, (uint32_t)1u << (uint32_t)src_reg);
    break;
  }

  case MACH_OP_CHAIN_REL:
  {
    /* Captured variable: store to parent frame via static chain. */
    ScratchRegAlloc chain_scratch = {0};
    int chain_used = 0;
    int base = resolve_chain_base(tcc_state->ir, dest.u.chain.chain_index, (1u << (uint32_t)src_reg), &chain_scratch,
                                  &chain_used);
    int32_t off = dest.u.chain.offset;
    int sign = (off < 0), abs_off = sign ? (int)(-off) : (int)off;
    if (btype == IROP_BTYPE_INT8)
      th_store8_imm_or_reg(src_reg, (uint32_t)base, abs_off, sign);
    else if (btype == IROP_BTYPE_INT16)
      th_store16_imm_or_reg(src_reg, (uint32_t)base, abs_off, sign);
    else
      th_store32_imm_or_reg_ex(src_reg, (uint32_t)base, abs_off, sign,
                               (1u << (uint32_t)src_reg) | (1u << (uint32_t)base));
    if (chain_used)
      restore_scratch_reg(&chain_scratch);
    break;
  }

  default:
    tcc_error("compiler_error: store_mop: unhandled dest kind %d", (int)dest.kind);
  }

  mach_release_all(&ctx);
}

/* Indexed load: dest = *(base + (index << scale))
 * Generates: LDR dest, [base, index, LSL #scale]
 */
ST_FUNC void tcc_gen_machine_load_indexed_mop(MachineOperand dest, MachineOperand base, MachineOperand index,
                                              MachineOperand scale, TccIrOp op)
{
  MachineCodegenContext ctx = {0};
  (void)op;

  int shift_amount = (scale.kind == MACH_OP_IMM) ? (int)scale.u.imm.val : 2;
  if (shift_amount < 0 || shift_amount > 31)
    shift_amount = 2;

  /* Fast path: base is &local + constant index — fold into SP/FP-relative load.
   * Mirrors the store_indexed FRAME_ADDR fast path. */
  if (!dest.is_64bit && shift_amount == 0 && index.kind == MACH_OP_IMM &&
      base.kind == MACH_OP_FRAME_ADDR && !base.needs_deref)
  {
    int combined = base.u.frame.offset + (int)index.u.imm.val;
    int adjusted = fp_adjust_local_offset(combined, 0);
    int sign = (adjusted < 0);
    int abs_off = sign ? -adjusted : adjusted;
    if (abs_off <= 4095)
    {
      const int dest_reg = mach_get_dest_reg(&ctx, &dest, 0);
      const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
      load_from_base(dest_reg, PREG_REG_NONE, dest.btype, (int)dest.is_unsigned, abs_off, sign, (uint32_t)base_reg);
      mach_writeback_dest(&dest, dest_reg);
      mach_release_all(&ctx);
      return;
    }
  }

  /* Fast path: constant-displacement load (scale == 0 and index is an immediate).
   * Generated by the displacement-fusion pass when folding `ADD base,#imm; LOAD *`
   * into a single `LDR dest,[base,#imm]`, matching GCC's addressing-mode output. */
  if (!dest.is_64bit && shift_amount == 0 && index.kind == MACH_OP_IMM)
  {
    int imm = (int)index.u.imm.val;
    int sign = (imm < 0);
    int abs_off = sign ? -imm : imm;
    if (abs_off <= 4095)
    {
      const int dest_reg = mach_get_dest_reg(&ctx, &dest, 0);
      uint32_t excl = (1u << (uint32_t)dest_reg);
      int base_reg = mach_ensure_in_reg(&ctx, &base, excl);
      load_from_base(dest_reg, PREG_REG_NONE, dest.btype, (int)dest.is_unsigned, abs_off, sign, (uint32_t)base_reg);
      mach_writeback_dest(&dest, dest_reg);
      mach_release_all(&ctx);
      return;
    }
  }

  /* scale 0 → no shift: use THUMB_SHIFT_NONE so the 16-bit T1 register-offset
   * encoding (all-low regs) can be selected instead of the wide T32 form. */
  thumb_shift shift = (shift_amount == 0)
                          ? (thumb_shift){.type = THUMB_SHIFT_NONE, .value = 0, .mode = THUMB_SHIFT_IMMEDIATE}
                          : (thumb_shift){.type = THUMB_SHIFT_LSL, .value = (uint32_t)shift_amount, .mode = THUMB_SHIFT_IMMEDIATE};

  /* Fast path: 64-bit constant-displacement load using LDRD [base, #imm].
   * LDRD supports word-aligned offsets in range [-1020, 1020]. */
  if (dest.is_64bit && shift_amount == 0 && index.kind == MACH_OP_IMM)
  {
    int imm = (int)index.u.imm.val;
    int sign = (imm < 0);
    int abs_off = sign ? -imm : imm;
    if (abs_off <= 1020 && (abs_off & 3) == 0)
    {
      const bool dest_is_reg = (dest.kind == MACH_OP_REG && !dest.needs_deref);
      int dest_lo, dest_hi;
      MachineOperand dest_hi_mop = {0};
      uint32_t excl = 0;

      if (dest_is_reg)
      {
        dest_lo = dest.u.reg.r0;
        if (!thumb_is_hw_reg(dest.u.reg.r1))
          tcc_error("load_indexed_mop: 64-bit dest has invalid r1=%d (r0=%d) — "
                    "register allocator must produce a valid pair",
                    dest.u.reg.r1, dest.u.reg.r0);
        dest_hi = dest.u.reg.r1;
        excl = (1u << (uint32_t)dest_lo) | (1u << (uint32_t)dest_hi);
      }
      else
      {
        dest_lo = mach_get_dest_reg(&ctx, &dest, 0);
        excl = (1u << (uint32_t)dest_lo);
        dest_hi_mop = mach_make_hi_half(&dest);
        dest_hi = mach_get_dest_reg(&ctx, &dest_hi_mop, excl);
        excl |= (1u << (uint32_t)dest_hi);
      }

      int base_reg = mach_ensure_in_reg(&ctx, &base, excl);
      uint32_t puw = sign ? 4u : 6u;
      ot_check(th_ldrd_imm((uint32_t)dest_lo, (uint32_t)dest_hi, (uint32_t)base_reg, abs_off, puw));
      if (!dest_is_reg)
      {
        MachineOperand dest_lo_mop = mach_make_lo_half(&dest);
        mach_writeback_dest(&dest_lo_mop, dest_lo);
        mach_writeback_dest(&dest_hi_mop, dest_hi);
      }
      mach_release_all(&ctx);
      return;
    }
  }

  /* 64-bit indexed load: compute EA = base + index<<shift into scratch, then LDRD. */
  if (dest.is_64bit)
  {
    const bool dest_is_reg = (dest.kind == MACH_OP_REG && !dest.needs_deref);
    int dest_lo, dest_hi;
    MachineOperand dest_hi_mop = {0};
    uint32_t excl = 0;

    if (dest_is_reg)
    {
      dest_lo = dest.u.reg.r0;
      if (!thumb_is_hw_reg(dest.u.reg.r1))
        tcc_error("load_indexed_mop: 64-bit dest has invalid r1=%d (r0=%d) — "
                  "register allocator must produce a valid pair",
                  dest.u.reg.r1, dest.u.reg.r0);
      dest_hi = dest.u.reg.r1;
      excl = (1u << (uint32_t)dest_lo) | (1u << (uint32_t)dest_hi);
    }
    else
    {
      dest_lo = mach_get_dest_reg(&ctx, &dest, 0);
      excl = (1u << (uint32_t)dest_lo);
      dest_hi_mop = mach_make_hi_half(&dest);
      dest_hi = mach_get_dest_reg(&ctx, &dest_hi_mop, excl);
      excl |= (1u << (uint32_t)dest_hi);
    }

    if (index.kind == MACH_OP_REG && !index.needs_deref)
      excl |= (1u << (uint32_t)index.u.reg.r0);
    int base_reg = mach_ensure_in_reg(&ctx, &base, excl);
    excl |= (1u << (uint32_t)base_reg);
    int index_reg = mach_ensure_in_reg(&ctx, &index, excl);
    excl |= (1u << (uint32_t)index_reg);
    int ea_r = mach_alloc_scratch(&ctx, excl);
    ot_check(th_add_reg((uint32_t)ea_r, (uint32_t)base_reg, (uint32_t)index_reg, flags_safe(), shift,
                        ENFORCE_ENCODING_NONE));
    ot_check(th_ldrd_imm((uint32_t)dest_lo, (uint32_t)dest_hi, (uint32_t)ea_r, 0, 5));
    if (!dest_is_reg)
    {
      MachineOperand dest_lo_mop = mach_make_lo_half(&dest);
      mach_writeback_dest(&dest_lo_mop, dest_lo);
      mach_writeback_dest(&dest_hi_mop, dest_hi);
    }
    mach_release_all(&ctx);
    return;
  }

  /* Use mach_get_dest_reg so MACH_OP_SPILL / MACH_OP_PARAM_STACK dests get a
   * scratch + writeback (previously `dest.u.reg.r0` was read unconditionally,
   * aliasing with spill.offset and emitting an invalid encoding). */
  const int dest_reg = mach_get_dest_reg(&ctx, &dest, 0);
  const int btype = dest.btype;
  const int is_unsigned = (int)dest.is_unsigned;

  uint32_t excl = (1u << (uint32_t)dest_reg);
  if (index.kind == MACH_OP_REG && !index.needs_deref)
    excl |= (1u << (uint32_t)index.u.reg.r0);
  int base_reg = mach_ensure_in_reg(&ctx, &base, excl);
  excl |= (1u << (uint32_t)base_reg);
  int index_reg = mach_ensure_in_reg(&ctx, &index, excl);

  if (btype == IROP_BTYPE_INT8)
  {
    if (is_unsigned)
      ot_check(th_ldrb_reg(dest_reg, base_reg, index_reg, shift, ENFORCE_ENCODING_NONE));
    else
      ot_check(th_ldrsb_reg(dest_reg, base_reg, index_reg, shift, ENFORCE_ENCODING_NONE));
  }
  else if (btype == IROP_BTYPE_INT16)
  {
    if (is_unsigned)
      ot_check(th_ldrh_reg(dest_reg, base_reg, index_reg, shift, ENFORCE_ENCODING_NONE));
    else
      ot_check(th_ldrsh_reg(dest_reg, base_reg, index_reg, shift, ENFORCE_ENCODING_NONE));
  }
  else
  {
    ot_check(th_ldr_reg(dest_reg, base_reg, index_reg, shift, ENFORCE_ENCODING_NONE));
  }
  mach_writeback_dest(&dest, dest_reg);
  mach_release_all(&ctx);
}

/* Indexed store: *(base + (index << scale)) = value
 * Generates: STR value, [base, index, LSL #scale]
 */
ST_FUNC void tcc_gen_machine_store_indexed_mop(MachineOperand base, MachineOperand index, MachineOperand scale,
                                               MachineOperand value, TccIrOp op)
{
  MachineCodegenContext ctx = {0};
  (void)op;

  int shift_amount = (scale.kind == MACH_OP_IMM) ? (int)scale.u.imm.val : 2;
  if (shift_amount < 0 || shift_amount > 31)
    shift_amount = 2;

  /* Fast path: base is &local + constant index — fold into SP/FP-relative store.
   * Avoids emitting a separate `ADD base, sp, #frame_off` LEA before the STR,
   * cutting one instruction per access in dense local-array initialization. */
  if (!value.is_64bit && shift_amount == 0 && index.kind == MACH_OP_IMM &&
      base.kind == MACH_OP_FRAME_ADDR && !base.needs_deref)
  {
    int combined = base.u.frame.offset + (int)index.u.imm.val;
    int adjusted = fp_adjust_local_offset(combined, 0);
    int sign = (adjusted < 0);
    int abs_off = sign ? -adjusted : adjusted;
    if (abs_off <= 4095)
    {
      const int btype = value.btype;
      const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
      int value_reg = mach_ensure_in_reg(&ctx, &value, 0);
      if (btype == IROP_BTYPE_INT8)
        th_store8_imm_or_reg(value_reg, (uint32_t)base_reg, abs_off, sign);
      else if (btype == IROP_BTYPE_INT16)
        th_store16_imm_or_reg(value_reg, (uint32_t)base_reg, abs_off, sign);
      else
        th_store32_imm_or_reg_ex(value_reg, (uint32_t)base_reg, abs_off, sign,
                                 (1u << (uint32_t)value_reg) | (1u << (uint32_t)base_reg));
      mach_release_all(&ctx);
      return;
    }
  }

  /* Fast path: constant-displacement store (scale == 0 and index is an immediate).
   * Mirrors the load_indexed fast path; emits `STR value,[base,#imm]`. */
  if (!value.is_64bit && shift_amount == 0 && index.kind == MACH_OP_IMM)
  {
    int imm = (int)index.u.imm.val;
    int sign = (imm < 0);
    int abs_off = sign ? -imm : imm;
    if (abs_off <= 4095)
    {
      const int btype = value.btype;
      int value_reg = mach_ensure_in_reg(&ctx, &value, 0);
      uint32_t excl = (1u << (uint32_t)value_reg);
      int base_reg = mach_ensure_in_reg(&ctx, &base, excl);
      if (btype == IROP_BTYPE_INT8)
        th_store8_imm_or_reg(value_reg, (uint32_t)base_reg, abs_off, sign);
      else if (btype == IROP_BTYPE_INT16)
        th_store16_imm_or_reg(value_reg, (uint32_t)base_reg, abs_off, sign);
      else
        th_store32_imm_or_reg_ex(value_reg, (uint32_t)base_reg, abs_off, sign,
                                 (1u << (uint32_t)value_reg) | (1u << (uint32_t)base_reg));
      mach_release_all(&ctx);
      return;
    }
  }

  /* scale 0 → no shift: use THUMB_SHIFT_NONE so the 16-bit T1 register-offset
   * encoding (all-low regs) can be selected instead of the wide T32 form. */
  thumb_shift shift = (shift_amount == 0)
                          ? (thumb_shift){.type = THUMB_SHIFT_NONE, .value = 0, .mode = THUMB_SHIFT_IMMEDIATE}
                          : (thumb_shift){.type = THUMB_SHIFT_LSL, .value = (uint32_t)shift_amount, .mode = THUMB_SHIFT_IMMEDIATE};

  /* Fast path: 64-bit constant-displacement store using STRD [base, #imm]. */
  if (value.is_64bit && shift_amount == 0 && index.kind == MACH_OP_IMM)
  {
    int imm = (int)index.u.imm.val;
    int sign = (imm < 0);
    int abs_off = sign ? -imm : imm;
    if (abs_off <= 1020 && (abs_off & 3) == 0)
    {
      MachineOperand val_lo = mach_make_lo_half(&value);
      val_lo.btype = IROP_BTYPE_INT32;
      MachineOperand val_hi = mach_make_hi_half(&value);
      val_hi.btype = IROP_BTYPE_INT32;
      const int lo_reg = mach_ensure_in_reg(&ctx, &val_lo, 0);
      uint32_t excl = (1u << (uint32_t)lo_reg);
      const int hi_reg = mach_ensure_in_reg(&ctx, &val_hi, excl);
      excl |= (1u << (uint32_t)hi_reg);
      int base_reg = mach_ensure_in_reg(&ctx, &base, excl);
      uint32_t puw = sign ? 4u : 6u;
      ot_check(th_strd_imm((uint32_t)lo_reg, (uint32_t)hi_reg, (uint32_t)base_reg, abs_off, puw));
      mach_release_all(&ctx);
      return;
    }
  }

  /* 64-bit indexed store: compute EA = base + index<<shift into scratch, then STRD. */
  if (value.is_64bit)
  {
    MachineOperand val_lo = mach_make_lo_half(&value);
    val_lo.btype = IROP_BTYPE_INT32;
    MachineOperand val_hi = mach_make_hi_half(&value);
    val_hi.btype = IROP_BTYPE_INT32;
    uint32_t excl = 0;
    if (base.kind == MACH_OP_REG && !base.needs_deref)
      excl |= (1u << (uint32_t)base.u.reg.r0);
    if (index.kind == MACH_OP_REG && !index.needs_deref)
      excl |= (1u << (uint32_t)index.u.reg.r0);
    const int lo_reg = mach_ensure_in_reg(&ctx, &val_lo, excl);
    excl |= (1u << (uint32_t)lo_reg);
    const int hi_reg = mach_ensure_in_reg(&ctx, &val_hi, excl);
    excl |= (1u << (uint32_t)hi_reg);
    int base_reg = mach_ensure_in_reg(&ctx, &base, excl);
    excl |= (1u << (uint32_t)base_reg);
    int index_reg = mach_ensure_in_reg(&ctx, &index, excl);
    excl |= (1u << (uint32_t)index_reg);
    int ea_r = mach_alloc_scratch(&ctx, excl);
    ot_check(th_add_reg((uint32_t)ea_r, (uint32_t)base_reg, (uint32_t)index_reg, flags_safe(), shift,
                        ENFORCE_ENCODING_NONE));
    ot_check(th_strd_imm((uint32_t)lo_reg, (uint32_t)hi_reg, (uint32_t)ea_r, 0, 6));
    mach_release_all(&ctx);
    return;
  }

  const int btype = value.btype;

  uint32_t excl = 0;
  if (base.kind == MACH_OP_REG && !base.needs_deref)
    excl |= (1u << (uint32_t)base.u.reg.r0);
  if (index.kind == MACH_OP_REG && !index.needs_deref)
    excl |= (1u << (uint32_t)index.u.reg.r0);
  int value_reg = mach_ensure_in_reg(&ctx, &value, excl);
  excl |= (1u << (uint32_t)value_reg);
  int base_reg = mach_ensure_in_reg(&ctx, &base, excl);
  excl |= (1u << (uint32_t)base_reg);
  int index_reg = mach_ensure_in_reg(&ctx, &index, excl);

  if (btype == IROP_BTYPE_INT8)
    ot_check(th_strb_reg(value_reg, base_reg, index_reg, shift, ENFORCE_ENCODING_NONE));
  else if (btype == IROP_BTYPE_INT16)
    ot_check(th_strh_reg(value_reg, base_reg, index_reg, shift, ENFORCE_ENCODING_NONE));
  else
    ot_check(th_str_reg(value_reg, base_reg, index_reg, shift, ENFORCE_ENCODING_NONE));

  mach_release_all(&ctx);
}

/* Post-increment load: dest = *ptr; ptr += offset
 * Generates: LDR dest, [ptr], #offset  (puw=3: post-index, add, writeback)
 */
ST_FUNC void tcc_gen_machine_load_postinc_mop(MachineOperand dest, MachineOperand ptr, MachineOperand offset,
                                              TccIrOp op)
{
  MachineCodegenContext ctx = {0};
  (void)op;

  int offset_imm = (offset.kind == MACH_OP_IMM) ? (int)offset.u.imm.val : 4;
  if (offset_imm < 0 || offset_imm > 255)
  {
    mach_release_all(&ctx);
    tcc_error("compiler_error: post-increment offset %d out of range (0-255)", offset_imm);
    return;
  }
  const uint32_t puw = 3; /* post-index (p=0), add (u=1), writeback (w=1) */

  /* 64-bit post-increment load: LDRD dest_lo, dest_hi, [ptr], #offset */
  if (dest.is_64bit)
  {
    const int dest_lo = mach_get_dest_reg(&ctx, &dest, 0);
    MachineOperand dest_hi_mop = mach_make_hi_half(&dest);
    const int dest_hi = mach_get_dest_reg(&ctx, &dest_hi_mop, (1u << (uint32_t)dest_lo));
    uint32_t excl = (1u << (uint32_t)dest_lo) | (1u << (uint32_t)dest_hi);
    int ptr_reg = mach_ensure_in_reg(&ctx, &ptr, excl);
    ot_check(
        th_ldrd_imm((uint32_t)dest_lo, (uint32_t)dest_hi, (uint32_t)ptr_reg, offset_imm, puw));
    mach_writeback_dest(&dest_hi_mop, dest_hi);
    mach_writeback_dest(&dest, dest_lo);
    mach_release_all(&ctx);
    return;
  }

  const int dest_reg = mach_get_dest_reg(&ctx, &dest, 0);
  const int btype = dest.btype;
  const int is_unsigned = (int)dest.is_unsigned;

  uint32_t excl = (1u << (uint32_t)dest_reg);
  int ptr_reg = mach_ensure_in_reg(&ctx, &ptr, excl);

  if (btype == IROP_BTYPE_INT8)
  {
    if (is_unsigned)
      ot_check(th_ldrb_imm(dest_reg, ptr_reg, offset_imm, puw, ENFORCE_ENCODING_NONE));
    else
      ot_check(th_ldrsb_imm(dest_reg, ptr_reg, offset_imm, puw, ENFORCE_ENCODING_NONE));
  }
  else if (btype == IROP_BTYPE_INT16)
  {
    if (is_unsigned)
      ot_check(th_ldrh_imm(dest_reg, ptr_reg, offset_imm, puw, ENFORCE_ENCODING_NONE));
    else
      ot_check(th_ldrsh_imm(dest_reg, ptr_reg, offset_imm, puw, ENFORCE_ENCODING_NONE));
  }
  else
  {
    ot_check_ldr_imm(dest_reg, ptr_reg, offset_imm, puw, ENFORCE_ENCODING_NONE);
  }
  mach_writeback_dest(&dest, dest_reg);
  mach_release_all(&ctx);
}

/* Post-increment store: *ptr = value; ptr += offset
 * Generates: STR value, [ptr], #offset  (puw=3: post-index, add, writeback)
 */
ST_FUNC void tcc_gen_machine_store_postinc_mop(MachineOperand ptr, MachineOperand value, MachineOperand offset,
                                               TccIrOp op)
{
  MachineCodegenContext ctx = {0};
  (void)op;

  int offset_imm = (offset.kind == MACH_OP_IMM) ? (int)offset.u.imm.val : 4;
  if (offset_imm < 0 || offset_imm > 255)
  {
    mach_release_all(&ctx);
    tcc_error("compiler_error: post-increment offset %d out of range (0-255)", offset_imm);
    return;
  }
  const uint32_t puw = 3; /* post-index (p=0), add (u=1), writeback (w=1) */

  /* 64-bit post-increment store: STRD lo, hi, [ptr], #offset */
  if (value.is_64bit)
  {
    MachineOperand val_lo = mach_make_lo_half(&value);
    val_lo.btype = IROP_BTYPE_INT32;
    MachineOperand val_hi = mach_make_hi_half(&value);
    val_hi.btype = IROP_BTYPE_INT32;
    const int lo_reg = mach_ensure_in_reg(&ctx, &val_lo, 0);
    uint32_t excl = (1u << (uint32_t)lo_reg);
    const int hi_reg = mach_ensure_in_reg(&ctx, &val_hi, excl);
    excl |= (1u << (uint32_t)hi_reg);
    int ptr_reg = mach_ensure_in_reg(&ctx, &ptr, excl);
    ot_check(
        th_strd_imm((uint32_t)lo_reg, (uint32_t)hi_reg, (uint32_t)ptr_reg, offset_imm, puw));
    mach_release_all(&ctx);
    return;
  }

  const int btype = value.btype;

  int value_reg = mach_ensure_in_reg(&ctx, &value, 0);
  uint32_t excl = (1u << (uint32_t)value_reg);
  int ptr_reg = mach_ensure_in_reg(&ctx, &ptr, excl);

  if (btype == IROP_BTYPE_INT8)
    ot_check(th_strb_imm(value_reg, ptr_reg, offset_imm, puw, ENFORCE_ENCODING_NONE));
  else if (btype == IROP_BTYPE_INT16)
    ot_check(th_strh_imm(value_reg, ptr_reg, offset_imm, puw, ENFORCE_ENCODING_NONE));
  else
    ot_check_str_imm(value_reg, ptr_reg, offset_imm, puw, ENFORCE_ENCODING_NONE);

  mach_release_all(&ctx);
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

/* fp_mop_load_arg: Load a MachineOperand value into a fixed argument register
 * (R0, R1, etc.) for a soft-float ABI call.  Unlike mach_ensure_in_reg, this
 * writes to a caller-specified register without scratch allocation bookkeeping.
 * Used by tcc_gen_machine_fp_mop to set up R0/R1 before BL __aeabi_f*. */
static void fp_mop_load_arg(int target_reg, const MachineOperand *op)
{
  switch (op->kind)
  {
  case MACH_OP_NONE:
    return;
  case MACH_OP_REG:
    if (!op->needs_deref)
    {
      if (op->u.reg.r0 != target_reg)
        ot_check_mov_reg((uint32_t)target_reg, (uint32_t)op->u.reg.r0, flags_safe(),
                         THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
    }
    else
    {
      load_from_base(target_reg, PREG_REG_NONE, op->btype, (int)op->is_unsigned, 0, 0, (uint32_t)op->u.reg.r0);
    }
    return;
  case MACH_OP_SPILL:
    tcc_machine_load_spill_slot(target_reg, op->u.spill.offset);
    if (op->needs_deref)
      load_from_base(target_reg, PREG_REG_NONE, op->btype, (int)op->is_unsigned, 0, 0, (uint32_t)target_reg);
    return;
  case MACH_OP_PARAM_STACK:
  {
    const int adjusted = op->u.param.offset + offset_to_args;
    const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
    const int sign = (adjusted < 0);
    const int abs_off = sign ? -adjusted : adjusted;
    load_from_base(target_reg, PREG_REG_NONE, op->btype, (int)op->is_unsigned, abs_off, sign, (uint32_t)base_reg);
    return;
  }
  case MACH_OP_IMM:
    tcc_machine_load_constant(target_reg, PREG_REG_NONE, op->u.imm.val, 0, NULL);
    return;
  case MACH_OP_SYMBOL:
  {
    Sym *sym = op->u.sym.sym ? validate_sym_for_reloc(op->u.sym.sym) : NULL;
    if (!op->needs_deref)
    {
      tcc_machine_load_constant(target_reg, PREG_REG_NONE, op->u.sym.addend, 0, sym);
    }
    else
    {
      /* Load symbol address into target_reg, then dereference through it. */
      tcc_machine_load_constant(target_reg, PREG_REG_NONE, 0, 0, sym);
      const int32_t addend = op->u.sym.addend;
      load_from_base(target_reg, PREG_REG_NONE, op->btype, (int)op->is_unsigned,
                     addend < 0 ? (int)(-addend) : (int)addend, addend < 0 ? 1 : 0, (uint32_t)target_reg);
    }
    return;
  }
  case MACH_OP_FRAME_ADDR:
    tcc_machine_addr_of_stack_slot(target_reg, op->u.frame.offset, 0);
    if (op->needs_deref)
      load_from_base(target_reg, PREG_REG_NONE, op->btype, (int)op->is_unsigned, 0, 0, (uint32_t)target_reg);
    return;
  case MACH_OP_CHAIN_REL:
  {
    /* Captured variable: load from parent frame via static chain. */
    ScratchRegAlloc chain_scratch = {0};
    int chain_used = 0;
    int base = resolve_chain_base(tcc_state->ir, op->u.chain.chain_index, (1u << (uint32_t)target_reg), &chain_scratch,
                                  &chain_used);
    int32_t off = op->u.chain.offset;
    int sign = (off < 0);
    int abs_off = sign ? (int)(-off) : (int)off;
    load_from_base(target_reg, PREG_REG_NONE, op->btype, (int)op->is_unsigned, abs_off, sign, (uint32_t)base);
    if (chain_used)
      restore_scratch_reg(&chain_scratch);
    return;
  }
  default:
    tcc_error("compiler_error: fp_mop_load_arg: unhandled kind %d", (int)op->kind);
  }
}

/* Load a 64-bit (double-precision) MachineOperand into two consecutive argument
 * registers (lo_reg = low 32 bits, hi_reg = high 32 bits).
 * Handles REG pair, SPILL pair, PARAM_STACK, and deref'd REG. */
static void fp_mop_load_double_arg(int lo_reg, int hi_reg, const MachineOperand *op)
{
  if (op->needs_deref && op->kind == MACH_OP_REG)
  {
    /* Pointer in register: resolve [r0] and [r0+4] into physical regs first. */
    MachineCodegenContext mctx;
    memset(&mctx, 0, sizeof(mctx));
    uint32_t excl = (1u << (uint32_t)lo_reg) | (1u << (uint32_t)hi_reg);
    MachineOperand resolved = mach_resolve_deref_64(&mctx, op, &excl);
    mach_release_all(&mctx);
    MachineOperand lo_op = mach_make_lo_half(&resolved);
    MachineOperand hi_op = mach_make_hi_half(&resolved);
    fp_mop_load_arg(lo_reg, &lo_op);
    fp_mop_load_arg(hi_reg, &hi_op);
    return;
  }
  if (op->kind == MACH_OP_PARAM_STACK)
  {
    /* Stack parameter: low word at op->offset, high word at op->offset + 4. */
    fp_mop_load_arg(lo_reg, op);
    MachineOperand hi_op = *op;
    hi_op.u.param.offset += 4;
    fp_mop_load_arg(hi_reg, &hi_op);
    return;
  }
  /* REG (non-deref) or SPILL: split with lo/hi helpers. */
  {
    MachineOperand lo_op = mach_make_lo_half(op);
    MachineOperand hi_op = mach_make_hi_half(op);
    fp_mop_load_arg(lo_reg, &lo_op);
    fp_mop_load_arg(hi_reg, &hi_op);
  }
}

/* Issue a BL to a soft-float library function, saving/restoring R9+R12 in
 * text+data-separation (PIC) mode. */
static void fp_mop_do_bl(const char *func_name)
{
  Sym *sym = external_global_sym(tok_alloc_const(func_name), &func_old_type);
  MachineOperand func_mop = {0};
  func_mop.kind = MACH_OP_SYMBOL;
  func_mop.u.sym.sym = sym;
  func_mop.u.sym.addend = 0;
  if (text_and_data_separation)
    ot_check(th_push((uint16_t)((1 << R9) | (1 << R12))));
  gcall_or_jump_mop(0, func_mop);
  if (text_and_data_separation)
    ot_check(th_pop((uint16_t)((1 << R9) | (1 << R12))));
}

/* Write a soft-float call result back to dest.
 * Single-precision result is in R0; double-precision is in R0 (lo) : R1 (hi). */
static void fp_mop_writeback_result(const MachineOperand *dest, int is_double)
{
  if (is_double)
  {
    MachineOperand lo_dest = mach_make_lo_half(dest);
    MachineOperand hi_dest = mach_make_hi_half(dest);
    mach_writeback_dest(&lo_dest, R0);
    mach_writeback_dest(&hi_dest, R1);
  }
  else
    mach_writeback_dest(dest, R0);
}

/* ============================================================
 * Complex float MOP path — Phase 5k
 * ============================================================
 *
 * Complex floats are 64-bit register pairs: lo = real, hi = imaginary.
 * Complex doubles are 128-bit values (always spilled): real at offset+0, imag at offset+8.
 * These functions use the MOP infrastructure (fp_mop_load_arg, fp_mop_do_bl,
 * mach_writeback_dest) to handle any operand kind (REG, SPILL, PARAM_STACK,
 * CHAIN_REL, etc.) without requiring fill_registers_ir.
 *
 * Strategy: save all inputs to a stack frame, call __aeabi_f* / __aeabi_d*
 * library functions, write results back to dest via mach_writeback_dest.
 */

/* Split a complex MachineOperand into its real component.
 * For complex float: real is the 32-bit lo half (same as mach_make_lo_half).
 * For complex double: real is the 64-bit double at the base offset. */
static MachineOperand mach_make_complex_real(const MachineOperand *op)
{
  if (op->btype == IROP_BTYPE_FLOAT64)
  {
    /* Complex double: real part is a 64-bit double at the base offset. */
    MachineOperand real = *op;
    real.is_complex = false;
    real.is_64bit = true; /* each component is 64-bit double */
    if (real.kind == MACH_OP_REG)
      ; /* keep r0:r1 pair — only valid for register-allocated complex floats */
    return real;
  }
  /* Complex float: fall back to lo half. */
  return mach_make_lo_half(op);
}

/* Split a complex MachineOperand into its imaginary component.
 * For complex float: imag is the 32-bit hi half (same as mach_make_hi_half).
 * For complex double: imag is the 64-bit double at base offset + 8. */
static MachineOperand mach_make_complex_imag(const MachineOperand *op)
{
  if (op->btype == IROP_BTYPE_FLOAT64)
  {
    /* Complex double: imag part is a 64-bit double at offset + 8. */
    MachineOperand imag = *op;
    imag.is_complex = false;
    imag.is_64bit = true;
    switch (imag.kind)
    {
    case MACH_OP_SPILL:
      imag.u.spill.offset += 8;
      break;
    case MACH_OP_FRAME_ADDR:
      imag.u.frame.offset += 8;
      break;
    case MACH_OP_PARAM_STACK:
      imag.u.param.offset += 8;
      break;
    case MACH_OP_CHAIN_REL:
      imag.u.chain.offset += 8;
      break;
    case MACH_OP_SYMBOL:
      imag.u.sym.addend += 8;
      break;
    case MACH_OP_REG:
      /* Register-based complex double shouldn't happen (force-spilled),
       * but handle gracefully: imaginary part is not representable. */
      break;
    default:
      break;
    }
    return imag;
  }
  /* Complex float: fall back to hi half. */
  return mach_make_hi_half(op);
}

/* Helper: save a double from R0:R1 to SP-relative stack offset. */
static void fp_mop_save_double_to_sp(int off)
{
  ot_check_str_imm(R0, R_SP, off, 6, ENFORCE_ENCODING_NONE);
  ot_check_str_imm(R1, R_SP, off + 4, 6, ENFORCE_ENCODING_NONE);
}

/* Helper: load a double from SP-relative stack offset into (lo_reg, hi_reg). */
static void fp_mop_load_double_from_sp(int lo_reg, int hi_reg, int off)
{
  ot_check_ldr_imm(lo_reg, R_SP, off, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(hi_reg, R_SP, off + 4, 6, ENFORCE_ENCODING_NONE);
}

/* Process complex double multiplication via MachineOperands.
 * Handles all cases:
 *   scalar × complex:  a * (c+di) = ac + (ad)i
 *   complex × scalar:  (a+bi) * c = ac + (bc)i
 *   complex × complex: (a+bi) * (c+di) = (ac-bd) + (ad+bc)i
 *
 * Uses __aeabi_dmul, __aeabi_dadd, __aeabi_dsub for double-precision.
 * Double AEABI calling convention: R0:R1 = arg1, R2:R3 = arg2, result in R0:R1.
 */
static void thumb_process_complex_mul_double_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  int s1_complex = src1.is_complex;
  int s2_complex = src2.is_complex;

  MachineOperand d_real = mach_make_complex_real(&dest);
  MachineOperand d_imag = mach_make_complex_imag(&dest);

  if (!s1_complex && s2_complex)
  {
    /* scalar double × complex double: a * (c+di) = ac + (ad)i */
    MachineOperand s2_real = mach_make_complex_real(&src2);
    MachineOperand s2_imag = mach_make_complex_imag(&src2);

    /* Allocate 8 bytes to save the scalar 'a'. */
    ot_check(th_sub_imm(R_SP, R_SP, 8, flags_safe(), ENFORCE_ENCODING_NONE));

    /* Load scalar 'a' into R0:R1 and save to stack. */
    fp_mop_load_double_arg(R0, R1, &src1);
    fp_mop_save_double_to_sp(0);

    /* Compute a * c: load 'c' into R2:R3. R0:R1 already = 'a'. */
    fp_mop_load_double_arg(R2, R3, &s2_real);
    fp_mop_do_bl("__aeabi_dmul");
    /* R0:R1 = a*c → write to dest real. */
    fp_mop_writeback_result(&d_real, 1);

    /* Compute a * d: reload 'a' from stack, load 'd' into R2:R3. */
    fp_mop_load_double_from_sp(R0, R1, 0);
    fp_mop_load_double_arg(R2, R3, &s2_imag);
    fp_mop_do_bl("__aeabi_dmul");
    /* R0:R1 = a*d → write to dest imag. */
    fp_mop_writeback_result(&d_imag, 1);

    ot_check(th_add_imm(R_SP, R_SP, 8, flags_safe(), ENFORCE_ENCODING_NONE));
  }
  else if (s1_complex && !s2_complex)
  {
    /* complex double × scalar double: (a+bi) * c = ac + (bc)i */
    MachineOperand s1_real = mach_make_complex_real(&src1);
    MachineOperand s1_imag = mach_make_complex_imag(&src1);

    /* Allocate 8 bytes to save the scalar 'c'. */
    ot_check(th_sub_imm(R_SP, R_SP, 8, flags_safe(), ENFORCE_ENCODING_NONE));

    /* Load scalar 'c' into R0:R1 and save to stack. */
    fp_mop_load_double_arg(R0, R1, &src2);
    fp_mop_save_double_to_sp(0);

    /* Compute a * c. */
    fp_mop_load_double_arg(R0, R1, &s1_real);
    fp_mop_load_double_arg(R2, R3, &src2);
    fp_mop_do_bl("__aeabi_dmul");
    fp_mop_writeback_result(&d_real, 1);

    /* Compute b * c: reload 'c', load 'b'. */
    fp_mop_load_double_arg(R0, R1, &s1_imag);
    fp_mop_load_double_from_sp(R2, R3, 0);
    fp_mop_do_bl("__aeabi_dmul");
    fp_mop_writeback_result(&d_imag, 1);

    ot_check(th_add_imm(R_SP, R_SP, 8, flags_safe(), ENFORCE_ENCODING_NONE));
  }
  else
  {
    /* complex × complex: (a+bi)*(c+di) = (ac-bd) + (ad+bc)i
     *
     * Stack layout (48 bytes):
     *   [sp+40] = d  (imag of src2, 8 bytes)
     *   [sp+32] = c  (real of src2, 8 bytes)
     *   [sp+24] = b  (imag of src1, 8 bytes)
     *   [sp+16] = a  (real of src1, 8 bytes)
     *   [sp+8]  = scratch1 (8 bytes)
     *   [sp+0]  = scratch0 (8 bytes)
     */
    MachineOperand s1_real = mach_make_complex_real(&src1);
    MachineOperand s1_imag = mach_make_complex_imag(&src1);
    MachineOperand s2_real = mach_make_complex_real(&src2);
    MachineOperand s2_imag = mach_make_complex_imag(&src2);

    const int off_scratch0 = 0, off_scratch1 = 8;
    const int off_a = 16, off_b = 24, off_c = 32, off_d = 40;

    ot_check(th_sub_imm(R_SP, R_SP, 48, flags_safe(), ENFORCE_ENCODING_NONE));

    /* Save all 4 components to stack. */
    fp_mop_load_double_arg(R0, R1, &s1_real);
    fp_mop_save_double_to_sp(off_a);
    fp_mop_load_double_arg(R0, R1, &s1_imag);
    fp_mop_save_double_to_sp(off_b);
    fp_mop_load_double_arg(R0, R1, &s2_real);
    fp_mop_save_double_to_sp(off_c);
    fp_mop_load_double_arg(R0, R1, &s2_imag);
    fp_mop_save_double_to_sp(off_d);

    /* Step 1: ac → scratch0. */
    fp_mop_load_double_from_sp(R0, R1, off_a);
    fp_mop_load_double_from_sp(R2, R3, off_c);
    fp_mop_do_bl("__aeabi_dmul");
    fp_mop_save_double_to_sp(off_scratch0);

    /* Step 2: bd → scratch1. */
    fp_mop_load_double_from_sp(R0, R1, off_b);
    fp_mop_load_double_from_sp(R2, R3, off_d);
    fp_mop_do_bl("__aeabi_dmul");
    fp_mop_save_double_to_sp(off_scratch1);

    /* Step 3: real = ac - bd → scratch0. */
    fp_mop_load_double_from_sp(R0, R1, off_scratch0);
    fp_mop_load_double_from_sp(R2, R3, off_scratch1);
    fp_mop_do_bl("__aeabi_dsub");
    fp_mop_save_double_to_sp(off_scratch0);

    /* Step 4: ad → scratch1. */
    fp_mop_load_double_from_sp(R0, R1, off_a);
    fp_mop_load_double_from_sp(R2, R3, off_d);
    fp_mop_do_bl("__aeabi_dmul");
    fp_mop_save_double_to_sp(off_scratch1);

    /* Step 5: bc → off_a (reuse slot). */
    fp_mop_load_double_from_sp(R0, R1, off_b);
    fp_mop_load_double_from_sp(R2, R3, off_c);
    fp_mop_do_bl("__aeabi_dmul");
    fp_mop_save_double_to_sp(off_a);

    /* Step 6: imag = ad + bc → scratch1. */
    fp_mop_load_double_from_sp(R0, R1, off_scratch1);
    fp_mop_load_double_from_sp(R2, R3, off_a);
    fp_mop_do_bl("__aeabi_dadd");
    fp_mop_save_double_to_sp(off_scratch1);

    /* Write results back to dest. */
    fp_mop_load_double_from_sp(R0, R1, off_scratch0);
    fp_mop_writeback_result(&d_real, 1);
    fp_mop_load_double_from_sp(R0, R1, off_scratch1);
    fp_mop_writeback_result(&d_imag, 1);

    ot_check(th_add_imm(R_SP, R_SP, 48, flags_safe(), ENFORCE_ENCODING_NONE));
  }
}

/* complex_pair_writeback: Write a (real, imag) pair from two physical registers
 * into a split MachineOperand pair without clobbering.
 * Handles the case where d_lo's target register overlaps hi_reg (or vice versa)
 * by saving the clobbered value to R2 or R3 first. */
static void complex_pair_writeback(MachineOperand *d_lo, int lo_reg, MachineOperand *d_hi, int hi_reg)
{
  int lo_clobbers_hi = (d_lo->kind == MACH_OP_REG && d_lo->u.reg.r0 == hi_reg);
  int hi_clobbers_lo = (d_hi->kind == MACH_OP_REG && d_hi->u.reg.r0 == lo_reg);

  if (lo_clobbers_hi && hi_clobbers_lo)
  {
    /* Total swap: save hi to temp, then write both */
    int tmp = (lo_reg != R2 && hi_reg != R2) ? R2 : R3;
    ot_check_mov_reg((uint32_t)tmp, (uint32_t)hi_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                     ENFORCE_ENCODING_NONE, false);
    mach_writeback_dest(d_lo, lo_reg);
    mach_writeback_dest(d_hi, tmp);
  }
  else if (lo_clobbers_hi)
  {
    /* Lo writeback would clobber hi value; save hi first */
    int tmp = (lo_reg != R2 && hi_reg != R2) ? R2 : R3;
    ot_check_mov_reg((uint32_t)tmp, (uint32_t)hi_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                     ENFORCE_ENCODING_NONE, false);
    mach_writeback_dest(d_lo, lo_reg);
    mach_writeback_dest(d_hi, tmp);
  }
  else if (hi_clobbers_lo)
  {
    /* Hi writeback would clobber lo value; write lo first */
    mach_writeback_dest(d_lo, lo_reg);
    mach_writeback_dest(d_hi, hi_reg);
  }
  else
  {
    mach_writeback_dest(d_lo, lo_reg);
    mach_writeback_dest(d_hi, hi_reg);
  }
}

/* Process complex double addition/subtraction via MachineOperands.
 * (a+bi) + (c+di) = (a+c) + (b+d)i
 * (a+bi) - (c+di) = (a-c) + (b-d)i
 * Uses __aeabi_dadd/__aeabi_dsub for double-precision.
 * Double AEABI calling convention: R0:R1 = arg1, R2:R3 = arg2, result in R0:R1.
 */
static void thumb_process_complex_op_double_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest,
                                                TccIrOp op)
{
  const int is_add = (op == TCCIR_OP_ADD);
  const char *func_name = is_add ? "__aeabi_dadd" : "__aeabi_dsub";

  MachineOperand s1_real = mach_make_complex_real(&src1);
  MachineOperand s1_imag = mach_make_complex_imag(&src1);
  MachineOperand s2_real = mach_make_complex_real(&src2);
  MachineOperand s2_imag = mach_make_complex_imag(&src2);
  MachineOperand d_real = mach_make_complex_real(&dest);
  MachineOperand d_imag = mach_make_complex_imag(&dest);

  /* Stack layout (32 bytes):
   *   [sp+24] = s2_imag (8 bytes)
   *   [sp+16] = s2_real (8 bytes)
   *   [sp+8]  = s1_imag (8 bytes)
   *   [sp+0]  = s1_real (8 bytes)
   */
  ot_check(th_sub_imm(R_SP, R_SP, 32, flags_safe(), ENFORCE_ENCODING_NONE));

  /* Save all 4 components to stack. */
  fp_mop_load_double_arg(R0, R1, &s1_real);
  fp_mop_save_double_to_sp(0);
  fp_mop_load_double_arg(R0, R1, &s1_imag);
  fp_mop_save_double_to_sp(8);
  fp_mop_load_double_arg(R0, R1, &s2_real);
  fp_mop_save_double_to_sp(16);
  fp_mop_load_double_arg(R0, R1, &s2_imag);
  fp_mop_save_double_to_sp(24);

  /* Compute real part: func(a.real, b.real) */
  fp_mop_load_double_from_sp(R0, R1, 0);
  fp_mop_load_double_from_sp(R2, R3, 16);
  fp_mop_do_bl(func_name);
  /* Save real result to stack slot 0 */
  fp_mop_save_double_to_sp(0);

  /* Compute imag part: func(a.imag, b.imag) */
  fp_mop_load_double_from_sp(R0, R1, 8);
  fp_mop_load_double_from_sp(R2, R3, 24);
  fp_mop_do_bl(func_name);
  /* R0:R1 = imag result. Load real result from stack. */
  fp_mop_save_double_to_sp(8); /* save imag to slot 8 */

  /* Write results back to dest. */
  fp_mop_load_double_from_sp(R0, R1, 0);
  fp_mop_writeback_result(&d_real, 1);
  fp_mop_load_double_from_sp(R0, R1, 8);
  fp_mop_writeback_result(&d_imag, 1);

  ot_check(th_add_imm(R_SP, R_SP, 32, flags_safe(), ENFORCE_ENCODING_NONE));
}

/* Process complex addition/subtraction via MachineOperands.
 * (a+bi) + (c+di) = (a+c) + (b+d)i
 * (a+bi) - (c+di) = (a-c) + (b-d)i
 */
static void thumb_process_complex_op_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op)
{
  const int is_add = (op == TCCIR_OP_ADD);

  /* Complex float: each component is a 32-bit float. */
  const char *func_name = is_add ? "__aeabi_fadd" : "__aeabi_fsub";

  /* Split into real/imag components. */
  MachineOperand s1_real = mach_make_lo_half(&src1);
  MachineOperand s1_imag = mach_make_hi_half(&src1);
  MachineOperand s2_real = mach_make_lo_half(&src2);
  MachineOperand s2_imag = mach_make_hi_half(&src2);

  /* Stack-based: save all 4 inputs, do calls, write results back.
   * Stack layout (16 bytes):
   *   [sp+12] = s2_imag
   *   [sp+8]  = s2_real
   *   [sp+4]  = s1_imag
   *   [sp+0]  = s1_real
   */
  ot_check(th_sub_imm(R_SP, R_SP, 16, flags_safe(), ENFORCE_ENCODING_NONE));

  /* Load and save each component to stack. */
  fp_mop_load_arg(R0, &s1_real);
  ot_check_str_imm(R0, R_SP, 0, 6, ENFORCE_ENCODING_NONE);
  fp_mop_load_arg(R0, &s1_imag);
  ot_check_str_imm(R0, R_SP, 4, 6, ENFORCE_ENCODING_NONE);
  fp_mop_load_arg(R0, &s2_real);
  ot_check_str_imm(R0, R_SP, 8, 6, ENFORCE_ENCODING_NONE);
  fp_mop_load_arg(R0, &s2_imag);
  ot_check_str_imm(R0, R_SP, 12, 6, ENFORCE_ENCODING_NONE);

  /* Compute real part: func(a.real, b.real) */
  ot_check_ldr_imm(R0, R_SP, 0, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R1, R_SP, 8, 6, ENFORCE_ENCODING_NONE);
  fp_mop_do_bl(func_name);
  /* Save real result to stack slot 0 */
  ot_check_str_imm(R0, R_SP, 0, 6, ENFORCE_ENCODING_NONE);

  /* Compute imag part: func(a.imag, b.imag) */
  ot_check_ldr_imm(R0, R_SP, 4, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R1, R_SP, 12, 6, ENFORCE_ENCODING_NONE);
  fp_mop_do_bl(func_name);
  /* R0 = imag result */

  /* Load real result from stack, deallocate, write back. */
  MachineOperand d_real = mach_make_lo_half(&dest);
  MachineOperand d_imag = mach_make_hi_half(&dest);

  /* R0 = imag result.  Load real result from stack into R1. */
  ot_check_ldr_imm(R1, R_SP, 0, 6, ENFORCE_ENCODING_NONE);
  ot_check(th_add_imm(R_SP, R_SP, 16, flags_safe(), ENFORCE_ENCODING_NONE));

  /* Write back: R1 = real part, R0 = imag part.
   * Use safe writeback to avoid clobbering when dest overlaps R0/R1. */
  complex_pair_writeback(&d_real, R1, &d_imag, R0);
}

/* Process complex multiplication via MachineOperands.
 * (a+bi) * (c+di) = (ac-bd) + (ad+bc)i
 *
 * Stack layout (24 bytes):
 *   [sp+20] = d  (imag of src2)
 *   [sp+16] = c  (real of src2)
 *   [sp+12] = b  (imag of src1)
 *   [sp+8]  = a  (real of src1)
 *   [sp+4]  = scratch1
 *   [sp+0]  = scratch0
 */
static void thumb_process_complex_mul_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  MachineOperand s1_real = mach_make_lo_half(&src1);
  MachineOperand s1_imag = mach_make_hi_half(&src1);
  MachineOperand s2_real = mach_make_lo_half(&src2);
  MachineOperand s2_imag = mach_make_hi_half(&src2);

  /* Allocate 24 bytes on stack */
  ot_check(th_sub_imm(R_SP, R_SP, 24, flags_safe(), ENFORCE_ENCODING_NONE));

  /* Save inputs to stack */
  fp_mop_load_arg(R0, &s1_real);
  ot_check_str_imm(R0, R_SP, 8, 6, ENFORCE_ENCODING_NONE); /* a */
  fp_mop_load_arg(R0, &s1_imag);
  ot_check_str_imm(R0, R_SP, 12, 6, ENFORCE_ENCODING_NONE); /* b */
  fp_mop_load_arg(R0, &s2_real);
  ot_check_str_imm(R0, R_SP, 16, 6, ENFORCE_ENCODING_NONE); /* c */
  fp_mop_load_arg(R0, &s2_imag);
  ot_check_str_imm(R0, R_SP, 20, 6, ENFORCE_ENCODING_NONE); /* d */

  const int off_scratch0 = 0;
  const int off_scratch1 = 4;
  const int off_a = 8;
  const int off_b = 12;
  const int off_c = 16;
  const int off_d = 20;

  /* Step 1: ac = a * c → scratch0 */
  ot_check_ldr_imm(R0, R_SP, off_a, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R1, R_SP, off_c, 6, ENFORCE_ENCODING_NONE);
  fp_mop_do_bl("__aeabi_fmul");
  ot_check_str_imm(R0, R_SP, off_scratch0, 6, ENFORCE_ENCODING_NONE);

  /* Step 2: bd = b * d → scratch1 */
  ot_check_ldr_imm(R0, R_SP, off_b, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R1, R_SP, off_d, 6, ENFORCE_ENCODING_NONE);
  fp_mop_do_bl("__aeabi_fmul");
  ot_check_str_imm(R0, R_SP, off_scratch1, 6, ENFORCE_ENCODING_NONE);

  /* Step 3: real = ac - bd → scratch0 */
  ot_check_ldr_imm(R0, R_SP, off_scratch0, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R1, R_SP, off_scratch1, 6, ENFORCE_ENCODING_NONE);
  fp_mop_do_bl("__aeabi_fsub");
  ot_check_str_imm(R0, R_SP, off_scratch0, 6, ENFORCE_ENCODING_NONE);

  /* Step 4: ad = a * d → scratch1 */
  ot_check_ldr_imm(R0, R_SP, off_a, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R1, R_SP, off_d, 6, ENFORCE_ENCODING_NONE);
  fp_mop_do_bl("__aeabi_fmul");
  ot_check_str_imm(R0, R_SP, off_scratch1, 6, ENFORCE_ENCODING_NONE);

  /* Step 5: bc = b * c → off_a (no longer needed) */
  ot_check_ldr_imm(R0, R_SP, off_b, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R1, R_SP, off_c, 6, ENFORCE_ENCODING_NONE);
  fp_mop_do_bl("__aeabi_fmul");
  ot_check_str_imm(R0, R_SP, off_a, 6, ENFORCE_ENCODING_NONE);

  /* Step 6: imag = ad + bc → scratch1 */
  ot_check_ldr_imm(R0, R_SP, off_scratch1, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R1, R_SP, off_a, 6, ENFORCE_ENCODING_NONE);
  fp_mop_do_bl("__aeabi_fadd");
  ot_check_str_imm(R0, R_SP, off_scratch1, 6, ENFORCE_ENCODING_NONE);

  /* Load results and write back */
  MachineOperand d_real = mach_make_lo_half(&dest);
  MachineOperand d_imag = mach_make_hi_half(&dest);
  ot_check_ldr_imm(R0, R_SP, off_scratch0, 6, ENFORCE_ENCODING_NONE); /* real */
  ot_check_ldr_imm(R1, R_SP, off_scratch1, 6, ENFORCE_ENCODING_NONE); /* imag */
  ot_check(th_add_imm(R_SP, R_SP, 24, flags_safe(), ENFORCE_ENCODING_NONE));

  complex_pair_writeback(&d_real, R0, &d_imag, R1);
}

/* Process complex float division via MachineOperands.
 * Calls __divsc3 from libgcc for numerically robust division.
 *
 * __divsc3 calling convention (soft-float AAPCS, hidden return pointer):
 *   R0       = hidden return pointer (8-byte buffer for result)
 *   R1       = a_real (first float arg)
 *   R2       = a_imag (second float arg)
 *   R3       = b_real (third float arg)
 *   [sp+0]   = b_imag (fourth float arg, on stack)
 *   Result written to [R0+0..3] = real, [R0+4..7] = imag
 *
 * Stack layout (16 bytes):
 *   [sp+0]   = b_imag for __divsc3 stack arg  (4 bytes)
 *   [sp+4]   = padding                        (4 bytes)
 *   [sp+8]   = result buffer: real part        (4 bytes)
 *   [sp+12]  = result buffer: imag part        (4 bytes)
 */
static void thumb_process_complex_div_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  MachineOperand s1_real = mach_make_lo_half(&src1);
  MachineOperand s1_imag = mach_make_hi_half(&src1);
  MachineOperand s2_real = mach_make_lo_half(&src2);
  MachineOperand s2_imag = mach_make_hi_half(&src2);

  /* In PIC mode, save {r9, r12} BEFORE allocating the call frame so that
   * SP-relative offsets within the 16-byte area remain correct when
   * __divsc3 reads its stack arg at [sp+0]. */
  if (text_and_data_separation)
    ot_check(th_push((uint16_t)((1 << R9) | (1 << R12))));

  /* Allocate 16 bytes on stack. */
  ot_check(th_sub_imm(R_SP, R_SP, 16, flags_safe(), ENFORCE_ENCODING_NONE));

  /* Save all inputs to stack first to avoid register clobbering. */
  fp_mop_load_arg(R0, &s1_real);
  ot_check_str_imm(R0, R_SP, 0, 6, ENFORCE_ENCODING_NONE); /* a_real */
  fp_mop_load_arg(R0, &s1_imag);
  ot_check_str_imm(R0, R_SP, 4, 6, ENFORCE_ENCODING_NONE); /* a_imag */
  fp_mop_load_arg(R0, &s2_real);
  ot_check_str_imm(R0, R_SP, 8, 6, ENFORCE_ENCODING_NONE); /* b_real */
  fp_mop_load_arg(R0, &s2_imag);
  ot_check_str_imm(R0, R_SP, 12, 6, ENFORCE_ENCODING_NONE); /* b_imag */

  /* Rearrange stack for __divsc3 call:
   * Need [sp+0] = b_imag, [sp+8..15] = result buffer.
   * Currently [sp+0]=a_real, [sp+4]=a_imag, [sp+8]=b_real, [sp+12]=b_imag.
   * Load R1-R3 from stack, then rearrange. */
  ot_check_ldr_imm(R1, R_SP, 0, 6, ENFORCE_ENCODING_NONE);  /* R1 = a_real */
  ot_check_ldr_imm(R2, R_SP, 4, 6, ENFORCE_ENCODING_NONE);  /* R2 = a_imag */
  ot_check_ldr_imm(R3, R_SP, 8, 6, ENFORCE_ENCODING_NONE);  /* R3 = b_real */
  ot_check_ldr_imm(R0, R_SP, 12, 6, ENFORCE_ENCODING_NONE); /* R0 = b_imag */
  ot_check_str_imm(R0, R_SP, 0, 6, ENFORCE_ENCODING_NONE);  /* [sp+0] = b_imag (stack arg) */

  /* R0 = pointer to result buffer at [sp+8]. */
  ot_check(th_add_imm(R0, R_SP, 8, flags_safe(), ENFORCE_ENCODING_NONE));

  /* Call __divsc3 (directly, not via fp_mop_do_bl which would add another
   * push/pop of {r9, r12} and corrupt the stack arg layout). */
  {
    Sym *sym = external_global_sym(tok_alloc_const("__divsc3"), &func_old_type);
    MachineOperand func_mop = {0};
    func_mop.kind = MACH_OP_SYMBOL;
    func_mop.u.sym.sym = sym;
    func_mop.u.sym.addend = 0;
    gcall_or_jump_mop(0, func_mop);
  }

  /* Read result from buffer and write back to dest. */
  MachineOperand d_real = mach_make_lo_half(&dest);
  MachineOperand d_imag = mach_make_hi_half(&dest);
  ot_check_ldr_imm(R0, R_SP, 8, 6, ENFORCE_ENCODING_NONE);  /* real */
  ot_check_ldr_imm(R1, R_SP, 12, 6, ENFORCE_ENCODING_NONE); /* imag */
  ot_check(th_add_imm(R_SP, R_SP, 16, flags_safe(), ENFORCE_ENCODING_NONE));

  if (text_and_data_separation)
    ot_check(th_pop((uint16_t)((1 << R9) | (1 << R12))));

  complex_pair_writeback(&d_real, R0, &d_imag, R1);
}

/* Process complex double division via MachineOperands.
 * Calls __divdc3 from libgcc for numerically robust division.
 *
 * __divdc3 calling convention (soft-float AAPCS, hidden return pointer):
 *   R0       = hidden return pointer (16-byte buffer for result)
 *   R2:R3    = a_re (first double, even-aligned)
 *   [sp+0]   = a_im (second double, on stack)
 *   [sp+8]   = b_re (third double, on stack)
 *   [sp+16]  = b_im (fourth double, on stack)
 *   Result written to [R0+0..7] = real, [R0+8..15] = imag
 *
 * Stack layout (40 bytes, 8-byte aligned):
 *   [sp+0]   = a_im for __divdc3 stack arg  (8 bytes)
 *   [sp+8]   = b_re for __divdc3 stack arg  (8 bytes)
 *   [sp+16]  = b_im for __divdc3 stack arg  (8 bytes)
 *   [sp+24]  = result buffer: real part      (8 bytes)
 *   [sp+32]  = result buffer: imag part      (8 bytes)
 */
static void thumb_process_complex_div_double_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  MachineOperand s1_real = mach_make_complex_real(&src1);
  MachineOperand s1_imag = mach_make_complex_imag(&src1);
  MachineOperand s2_real = mach_make_complex_real(&src2);
  MachineOperand s2_imag = mach_make_complex_imag(&src2);
  MachineOperand d_real = mach_make_complex_real(&dest);
  MachineOperand d_imag = mach_make_complex_imag(&dest);

  /* In PIC mode, save {r9, r12} BEFORE allocating the call frame so that
   * SP-relative offsets within the 40-byte area remain correct when
   * __divdc3 reads its stack args at [sp+0..23]. */
  if (text_and_data_separation)
    ot_check(th_push((uint16_t)((1 << R9) | (1 << R12))));

  /* Allocate 40 bytes (8-byte aligned). */
  ot_check(th_sub_imm(R_SP, R_SP, 40, flags_safe(), ENFORCE_ENCODING_NONE));

  /* Set up __divdc3 stack args (must be at lowest sp offsets). */
  /* [sp+16] = b_im (src2 imag). */
  fp_mop_load_double_arg(R0, R1, &s2_imag);
  fp_mop_save_double_to_sp(16);
  /* [sp+8] = b_re (src2 real). */
  fp_mop_load_double_arg(R0, R1, &s2_real);
  fp_mop_save_double_to_sp(8);
  /* [sp+0] = a_im (src1 imag). */
  fp_mop_load_double_arg(R0, R1, &s1_imag);
  fp_mop_save_double_to_sp(0);

  /* R2:R3 = a_re (src1 real) — first double arg in even register pair. */
  fp_mop_load_double_arg(R2, R3, &s1_real);

  /* R0 = pointer to result buffer at [sp+24]. */
  ot_check(th_add_imm(R0, R_SP, 24, flags_safe(), ENFORCE_ENCODING_NONE));

  /* Call __divdc3 (directly, not via fp_mop_do_bl which would add another
   * push/pop of {r9, r12} and corrupt the stack arg layout). */
  {
    Sym *sym = external_global_sym(tok_alloc_const("__divdc3"), &func_old_type);
    MachineOperand func_mop = {0};
    func_mop.kind = MACH_OP_SYMBOL;
    func_mop.u.sym.sym = sym;
    func_mop.u.sym.addend = 0;
    gcall_or_jump_mop(0, func_mop);
  }

  /* Read result from buffer and write back to dest. */
  fp_mop_load_double_from_sp(R0, R1, 24);
  fp_mop_writeback_result(&d_real, 1);
  fp_mop_load_double_from_sp(R0, R1, 32);
  fp_mop_writeback_result(&d_imag, 1);

  ot_check(th_add_imm(R_SP, R_SP, 40, flags_safe(), ENFORCE_ENCODING_NONE));

  if (text_and_data_separation)
    ot_check(th_pop((uint16_t)((1 << R9) | (1 << R12))));
}

/* tcc_gen_machine_fp_mop: MachineOperand-based entry point for floating-point
 * operations via soft-float EABI library calls.
 * Handles single-precision, double-precision, and complex float operations.
 *
 * Soft-float EABI calling convention (single-precision):
 *   binary arithmetic:  src1 → R0, src2 → R1, result ← R0
 *   comparison:         src1 → R0, src2 → R1, result ← CPSR flags
 *   negation:           src1 → R0, XOR sign bit, result ← R0
 *   conversion:         src1 → R0, result ← R0
 *   CVT_FTOF identity:  float32→float32, src1 → R0, dest ← R0
 */
ST_FUNC void tcc_gen_machine_fp_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op,
                                    int is_complex)
{
  /* Phase 5k: handle complex float operations via MOP path. */
  if (is_complex)
  {
    /* Detect double-precision complex: any operand has FLOAT64 btype. */
    const int complex_is_double =
        (src1.btype == IROP_BTYPE_FLOAT64 || src2.btype == IROP_BTYPE_FLOAT64 || dest.btype == IROP_BTYPE_FLOAT64);
    if (op == TCCIR_OP_FADD || op == TCCIR_OP_FSUB)
    {
      if (complex_is_double)
        return thumb_process_complex_op_double_mop(src1, src2, dest, op == TCCIR_OP_FADD ? TCCIR_OP_ADD : TCCIR_OP_SUB);
      return thumb_process_complex_op_mop(src1, src2, dest, op == TCCIR_OP_FADD ? TCCIR_OP_ADD : TCCIR_OP_SUB);
    }
    else if (op == TCCIR_OP_FMUL)
    {
      if (complex_is_double)
        return thumb_process_complex_mul_double_mop(src1, src2, dest);
      return thumb_process_complex_mul_mop(src1, src2, dest);
    }
    else if (op == TCCIR_OP_FDIV)
    {
      if (complex_is_double)
        return thumb_process_complex_div_double_mop(src1, src2, dest);
      return thumb_process_complex_div_mop(src1, src2, dest);
    }
    /* Other ops (FNEG, FCMP, CVT_*) on complex types: fall through to
     * scalar path — they operate componentwise on the lo (real) half only,
     * same as regular scalars.  TODO: extend if needed. */
  }

  /* is_double: true when the primary operand is a 64-bit float (double).
   * Note: complex float has is_64bit=true (register pair), but its btype
   * is FLOAT32, so it is NOT double.  Only FLOAT64 btype is true double. */
  const int is_double = (src1.btype == IROP_BTYPE_FLOAT64) || (dest.btype == IROP_BTYPE_FLOAT64);
  const char *func_name = NULL;

  /* --- FNEG: XOR sign bit, no BL needed --- */
  if (op == TCCIR_OP_FNEG)
  {
    ScratchRegAlloc scr;
    if (is_double)
    {
      /* f64: load pair into R0:R1, flip sign bit of hi word (R1) only */
      fp_mop_load_double_arg(R0, R1, &src1);
      scr = get_scratch_reg_with_save((1u << R0) | (1u << R1));
      load_full_const(scr.reg, PREG_NONE, 0x80000000, 0);
      ot_check(th_eor_reg(R1, R1, scr.reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      restore_scratch_reg(&scr);
      fp_mop_writeback_result(&dest, 1);
    }
    else
    {
      /* f32: R0 ^= 0x80000000 */
      fp_mop_load_arg(R0, &src1);
      scr = get_scratch_reg_with_save(1u << R0);
      load_full_const(scr.reg, PREG_NONE, 0x80000000, 0);
      ot_check(th_eor_reg(R0, R0, scr.reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      restore_scratch_reg(&scr);
      mach_writeback_dest(&dest, R0);
    }
    return;
  }

  /* --- CVT_FTOF: identity or f32<->f64 conversion via BL --- */
  if (op == TCCIR_OP_CVT_FTOF)
  {
    const int src_double = src1.is_64bit;
    const int dst_double = dest.is_64bit;
    if (!src_double && !dst_double)
    {
      /* f32 -> f32 identity: direct copy without going through R0 */
      tcc_gen_machine_assign_mop(src1, dest, TCCIR_OP_ASSIGN);
      return;
    }
    if (src_double && dst_double)
    {
      /* f64 -> f64 identity: direct copy without going through R0:R1.
       * Using R0:R1 as intermediaries would clobber live values in those
       * registers (e.g. function parameters in soft-float ABI). */
      tcc_gen_machine_assign_mop(src1, dest, TCCIR_OP_ASSIGN);
      return;
    }
    /* f32 -> f64: __aeabi_f2d;  f64 -> f32: __aeabi_d2f */
    {
      const char *cvt_func = src_double ? "__aeabi_d2f" : "__aeabi_f2d";
      if (src_double)
        fp_mop_load_double_arg(R0, R1, &src1);
      else
        fp_mop_load_arg(R0, &src1);
      fp_mop_do_bl(cvt_func);
      fp_mop_writeback_result(&dest, dst_double);
    }
    return;
  }

  /* --- Load operands into argument registers --- */
  if (op == TCCIR_OP_FCMP || op == TCCIR_OP_FADD || op == TCCIR_OP_FSUB || op == TCCIR_OP_FMUL || op == TCCIR_OP_FDIV)
  {
    /* Binary: src1 first arg, src2 second arg */
    if (is_double)
    {
      fp_mop_load_double_arg(R0, R1, &src1);
      fp_mop_load_double_arg(R2, R3, &src2);
    }
    else
    {
      fp_mop_load_arg(R0, &src1);
      fp_mop_load_arg(R1, &src2);
    }
  }
  else
  {
    /* Unary conversion (CVT_ITOF, CVT_FTOI): load src1 into R0 or R0:R1 */
    if (src1.is_64bit)
      fp_mop_load_double_arg(R0, R1, &src1);
    else
      fp_mop_load_arg(R0, &src1);
  }

  /* --- Determine soft-float function name --- */
  if (op == TCCIR_OP_FCMP)
  {
    func_name = is_double ? "__aeabi_cdcmple" : "__aeabi_cfcmple";
  }
  else if (op == TCCIR_OP_CVT_ITOF)
  {
    const int src64 = src1.is_64bit;
    const int dst64 = dest.is_64bit;
    if (src64 && dst64)
      func_name = src1.is_unsigned ? "__aeabi_ul2d" : "__aeabi_l2d";
    else if (src64)
      func_name = src1.is_unsigned ? "__aeabi_ul2f" : "__aeabi_l2f";
    else if (dst64)
      func_name = src1.is_unsigned ? "__aeabi_ui2d" : "__aeabi_i2d";
    else
      func_name = src1.is_unsigned ? "__aeabi_ui2f" : "__aeabi_i2f";
  }
  else if (op == TCCIR_OP_CVT_FTOI)
  {
    const int src64 = src1.is_64bit;
    const int dst64 = dest.is_64bit;
    if (src64 && dst64)
      func_name = dest.is_unsigned ? "__aeabi_d2ulz" : "__aeabi_d2lz";
    else if (src64)
      func_name = dest.is_unsigned ? "__aeabi_d2uiz" : "__aeabi_d2iz";
    else if (dst64)
      func_name = dest.is_unsigned ? "__aeabi_f2ulz" : "__aeabi_f2lz";
    else
      func_name = dest.is_unsigned ? "__aeabi_f2uiz" : "__aeabi_f2iz";
  }
  else
  {
    /* FADD, FSUB, FMUL, FDIV */
    func_name = get_softfp_func_name(op, is_double);
  }

  if (!func_name)
    tcc_error("compiler_error: tcc_gen_machine_fp_mop: no func_name for op %d", (int)op);

  fp_mop_do_bl(func_name);

  /* Write result back (FCMP sets CPSR flags only -- no register result) */
  if (op != TCCIR_OP_FCMP)
    fp_mop_writeback_result(&dest, dest.is_64bit);
}

/* tcc_gen_machine_return_value_mop: MachineOperand-based entry point for
 * function return.  Moves src into the return register(s) using the most
 * efficient sequence available:
 *   32-bit: src → R0
 *     - Already R0 (common after ASSIGN peephole): NOP, 0 scratch.
 *     - IMM / SYMBOL: load directly into R0, 0 scratch.
 *     - REG / SPILL / FRAME_ADDR / PARAM_STACK: mach_ensure_in_reg + optional MOV.
 *   64-bit: lo → R0 (REG_IRET), hi → R1 (REG_IRE2)
 *     - Delegates to tcc_gen_machine_assign_mop with a synthetic R0:R1 dest.
 *     - Safe ordering guaranteed: AAPCS ensures hi is never in R0.
 */
ST_FUNC void tcc_gen_machine_return_value_mop(MachineOperand src, TccIrOp op)
{
  (void)op;

  /* 64-bit return: lo word → R0 (REG_IRET), hi word → R1 (REG_IRE2).
   * AAPCS guarantees that for a 64-bit pair src.u.reg.r1 = src.u.reg.r0 + 1 ≥ R1,
   * so hi is never in R0.  Moving lo→R0 first is always safe.
   * delegate to assign_mop which handles REG/SPILL/IMM/SYMBOL src kinds. */
  if (src.is_64bit)
  {
    MachineOperand ret_pair;
    memset(&ret_pair, 0, sizeof(ret_pair));
    ret_pair.kind = MACH_OP_REG;
    ret_pair.u.reg.r0 = TREG_R0; /* REG_IRET */
    ret_pair.u.reg.r1 = TREG_R1; /* REG_IRE2 */
    ret_pair.is_64bit = true;
    ret_pair.btype = src.btype;
    tcc_gen_machine_assign_mop(src, ret_pair, op);
    return;
  }

  /* Fast path: value already in R0 (ASSIGN peephole sets dest→R0) */
  if (src.kind == MACH_OP_REG && !src.needs_deref && src.u.reg.r0 == R0)
    return;

  /* Immediate: materialize directly into R0 (no scratch register needed) */
  if (src.kind == MACH_OP_IMM)
  {
    tcc_machine_load_constant(R0, PREG_NONE, src.u.imm.val, 0, NULL);
    return;
  }

  /* Symbol: load address (+ optional deref) directly into R0 */
  if (src.kind == MACH_OP_SYMBOL)
  {
    Sym *sym = src.u.sym.sym ? validate_sym_for_reloc(src.u.sym.sym) : NULL;
    if (!src.needs_deref)
    {
      tcc_machine_load_constant(R0, PREG_NONE, src.u.sym.addend, 0, sym);
    }
    else
    {
      tcc_machine_load_constant(R0, PREG_NONE, 0, 0, sym);
      const int32_t addend = src.u.sym.addend;
      load_from_base(R0, PREG_REG_NONE, src.btype, (int)src.is_unsigned, addend < 0 ? (int)(-addend) : (int)addend,
                     addend < 0 ? 1 : 0, (uint32_t)R0);
    }
    return;
  }

  /* General case: materialize into any available register, then MOV to R0 */
  MachineCodegenContext ctx = {0};
  int src_reg = mach_ensure_in_reg(&ctx, &src, 0);
  if (src_reg != R0)
    ot_check_mov_reg(R0, src_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
  mach_release_all(&ctx);
}

ST_FUNC void tcc_gen_machine_prolog(int leaffunc, uint64_t used_registers, int stack_size, uint32_t extra_prologue_regs)
{
  thumb_gen_state.function_argument_count = 0;
  /* call_id -1 is reserved for function prolog metadata - but that doesn't
   * need to be stored in call_sites_by_id (which uses non-negative IDs).
   * If needed, handle it separately or skip. */

  thumb_gen_state.generating_function = 1;
  thumb_gen_state.code_size = 0;
  /* Clear global symbol cache at function start */
  thumb_gen_state.cached_global_sym = NULL;
  thumb_gen_state.cached_global_reg = PREG_NONE;
  /* MOV-coalescing cache is per-function: register live ranges don't
   * cross function boundaries. */
  mov_equiv_reset_all();
  TCCIRState *ir = tcc_state->ir;

  /* Determine if LR needs saving */
  int save_lr = !leaffunc || tcc_state->force_lr_save;
  if (extra_prologue_regs & (1u << R_LR))
    save_lr = 1;

  /* Variadic functions need a stable FP for va_list setup. */
  if (func_var)
    tcc_state->need_frame_pointer = 1;

  /* Also force FP when force_lr_save is set (builtin_return_address). */
  if (tcc_state->force_lr_save)
    tcc_state->need_frame_pointer = 1;

  const int need_fp = (tcc_state->force_frame_pointer || tcc_state->need_frame_pointer);
  tcc_state->need_frame_pointer = need_fp;

  /* Use two-phase push (standard frame record) when __builtin_return_address
   * needs a predictable {FP, LR} layout at [FP+0] and [FP+4]. */
  const int standard_frame_record = need_fp && tcc_state->force_lr_save;

  /* Collect callee-saved registers */
  uint16_t callee_regs_local = 0;
  int callee_count = 0;
  for (int i = R4; i <= R11; ++i)
  {
    if (tcc_state->text_and_data_separation && i == R9)
      continue;
    if (i == R_FP)
      continue; /* r7 handled separately for FP */
    if (used_registers & (1ULL << i))
    {
      callee_regs_local |= (1 << i);
      callee_count++;
    }
  }

  /* Add static chain register (R10) for nested functions. */
  if (extra_prologue_regs & (1u << ARM_R10))
  {
    if (!(callee_regs_local & (1u << ARM_R10)))
    {
      callee_regs_local |= (1u << ARM_R10);
      callee_count++;
    }
  }

  int push_align_pad = 0; /* 4 if push count is odd, absorbed into SUB SP */

  if (standard_frame_record)
  {
    /* ── Two-phase push: frame record {r7, lr} then callee-saved ──
     * Layout: [FP+0]=old_FP, [FP+4]=LR, callee-saved below FP. */
    uint16_t frame_regs = (1 << R_FP);
    int frame_count = 1;
    if (save_lr)
    {
      frame_regs |= (1 << R_LR);
      frame_count++;
    }

    /* Pad total to even count for 8-byte alignment (AAPCS).
     * Standard frame record uses FP-relative negative offsets (e.g. static
     * chain at [FP-4]), so the alignment gap must stay as SUB SP space
     * below FP — cannot use a dummy push register here. */
    int total = frame_count + callee_count;
    if (total % 2 != 0)
    {
      push_align_pad = 4;
    }

    th_sym_t();

    /* Variadic: push r0-r3 FIRST so they are contiguous with stack args */
    vararg_push_size = 0;
    if (func_var)
    {
      ot_check(th_push((1 << R0) | (1 << R1) | (1 << R2) | (1 << R3)));
      vararg_push_size = 16;
    }

    /* Phase A: push frame record */
    ot_check(th_push(frame_regs));

    /* MOV r7, sp — FP points at the frame record */
    if (!ot(th_add_imm(R_FP, R_SP, 0, flags_safe(), ENFORCE_ENCODING_NONE)))
    {
      fprintf(stderr, "compiler_error: prolog frame pointer setup failed\n");
      exit(1);
    }

    /* Phase B: push callee-saved regs (below FP) */
    if (callee_count > 0)
      ot_check(th_push(callee_regs_local));

    callee_push_size = callee_count * 4;
    callee_saved_regs = callee_regs_local;
    offset_to_args = frame_count * 4 + vararg_push_size;
    pushed_registers = frame_regs | callee_regs_local;
  }
  else
  {
    /* ── Original single-push layout ── */
    uint16_t registers_to_push = callee_regs_local;
    int registers_count = callee_count;

    if (save_lr)
    {
      registers_to_push |= (1 << R_LR);
      registers_count++;
    }
    if (need_fp)
    {
      registers_to_push |= (1 << R_FP);
      registers_count++;
    }

    /* Keep the total push size 8-byte aligned (AAPCS).
     * When no locals/FP (stack_size == 0, no frame pointer), pad by pushing
     * a dummy low register (R3) — avoids SUB SP + ADD SP, saving 2 insns.
     * When FP is used, alignment pad must stay as SUB SP space because
     * FP-relative negative offsets may address that area.
     * When locals exist, absorb the gap into SUB SP instead, keeping
     * PUSH in 16-bit encoding (no high regs like R12). */
    if (registers_count % 2 != 0)
    {
      if (stack_size == 0 && !need_fp)
      {
        registers_to_push |= (1 << R3);
        registers_count++;
      }
      else
      {
        push_align_pad = 4;
      }
    }

    th_sym_t();

    /* Variadic: push r0-r3 FIRST so they are contiguous with stack args */
    vararg_push_size = 0;
    if (func_var)
    {
      ot_check(th_push((1 << R0) | (1 << R1) | (1 << R2) | (1 << R3)));
      vararg_push_size = 16;
    }

    offset_to_args = registers_count * 4 + vararg_push_size;

    if (registers_count > 0)
      ot_check(th_push(registers_to_push));

    pushed_registers = registers_to_push;
    callee_push_size = 0;
    callee_saved_regs = 0;
  }

  // allocate stack space for local variables
  /* Variadic save area is reserved in the IR stack layout (loc bias). */

  /* Keep SP 8-byte aligned (AAPCS). tccir normally pre-aligns stack_size, but
   * be defensive here because other codepaths may call into the backend.
   */
  if (stack_size & 7)
    stack_size = (stack_size + 7) & ~7;

  /* allocated_stack_size is the portion of the stack used for locals/spills.
   * It must NOT include the alignment pad — local offsets are computed
   * relative to this value, and the pad sits below all addressable locals. */
  allocated_stack_size = stack_size;

  /* total_stack_dealloc is the full amount to restore in the epilogue,
   * including the alignment pad that sits below addressable locals. */
  int total_stack_dealloc = stack_size + push_align_pad;
  epilogue_stack_dealloc = total_stack_dealloc;
  stack_size = total_stack_dealloc;
  if (tcc_state->need_frame_pointer && !standard_frame_record)
  {
    if (!ot(th_add_imm(R_FP, R_SP, 0, flags_safe(), ENFORCE_ENCODING_NONE)))
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

  /* When FP is omitted, SP is lower by the full SUB SP amount (locals +
   * alignment pad).  Adjust offset_to_args so incoming stack parameters
   * are found at the correct SP-relative position. */
  if (!need_fp)
    offset_to_args += epilogue_stack_dealloc;

  /* However, local addressing uses allocated_stack_size (without pad).
   * The pad sits at the top of the SUB SP region, right below pushed regs,
   * so locals occupy SP+0 .. SP+allocated_stack_size-1, matching the same
   * addresses as the old push-IP-for-alignment approach. */

  /* Save incoming static chain (R10) at fixed chain slot.
   * With two-phase push, callee-saved regs are below FP, so the chain
   * slot is at [FP - callee_push_size - 4] instead of [FP - 4].
   * The body reads via offset -4 which gets fp_adjust_local_offset applied. */
  if (ir && ir->has_static_chain)
  {
    tcc_gen_machine_store_to_stack(architecture_config.static_chain_reg, -(callee_push_size + 4));
  }

  /* For variadic functions, save incoming r0-r3 in a fixed area at FP-16..FP-4
   * (for named parameter access) and store __gr_top at FP-20.
   * The PUSH {r0-r3} at function entry already creates a contiguous register
   * save area above the callee-saved pushes, adjacent to the stack arguments.
   * __gr_top points to the end of that area (= start of stack args).
   */
  int named_reg_bytes = 0;
  if (func_var && ir)
  {
    named_reg_bytes = ir->named_arg_reg_bytes;
  }

  if (func_var)
  {
    /* Store r0-r3 at FP-16..FP-4 for named parameter access.
     * (The contiguous PUSH'd copy is at FP+offset_to_args-16..FP+offset_to_args-4
     * and is used by va_arg for anonymous argument traversal.) */
    tcc_gen_machine_store_to_stack(R0, -(callee_push_size + 16));
    tcc_gen_machine_store_to_stack(R1, -(callee_push_size + 12));
    tcc_gen_machine_store_to_stack(R2, -(callee_push_size + 8));
    tcc_gen_machine_store_to_stack(R3, -(callee_push_size + 4));

    /* __gr_top = FP + offset_to_args (end of pushed r0-r3, start of stack args).
     * This is the top of the contiguous register save + stack arg area. */
    {
      const int fp_or_sp = tcc_state->need_frame_pointer ? R_FP : R_SP;
      ot_check(th_add_imm(R12, fp_or_sp, offset_to_args, flags_safe(), ENFORCE_ENCODING_NONE));
    }
    tcc_gen_machine_store_to_stack(R12, -(callee_push_size + 20));

    /* store the number of named-arg bytes consumed in r0-r3 */
    tcc_machine_load_constant(R12, PREG_NONE, named_reg_bytes, 0, NULL);
    tcc_gen_machine_store_to_stack(R12, -(callee_push_size + 24));

    /* store named stack arg bytes at FP-28 so __tcc_va_start can compute
     * __stack = __gr_top + named_stack_bytes (skipping named args on stack) */
    int named_stack_bytes = ir ? ir->named_arg_stack_bytes : 0;
    tcc_machine_load_constant(R12, PREG_NONE, named_stack_bytes, 0, NULL);
    tcc_gen_machine_store_to_stack(R12, -(callee_push_size + 28));
  }

  /* __builtin_apply_args: save incoming r0-r3 and stack args pointer
   * to the reserved apply_args block so __builtin_apply can replay them.
   * Layout at apply_args_offset: [stack_args_ptr, r0, r1, r2, r3]. */
  if (tcc_state->func_save_apply_args && ir)
  {
    int base_off = tcc_state->apply_args_offset;
    /* Adjust for callee push gap (same adjustment as fp_adjust_local_offset) */
    int adj = base_off;
    if (adj < 0 && callee_push_size > 0)
      adj -= callee_push_size;

    /* Store stack args pointer (FP + offset_to_args = start of stack args area) */
    {
      const int fp_or_sp = tcc_state->need_frame_pointer ? R_FP : R_SP;
      ot_check(th_add_imm(R_IP, fp_or_sp, offset_to_args, flags_safe(), ENFORCE_ENCODING_NONE));
    }
    tcc_gen_machine_store_to_stack_ex(R_IP, adj, (1u << R0) | (1u << R1) | (1u << R2) | (1u << R3));

    /* Store r0-r3 at offsets +4, +8, +12, +16 from the block start */
    tcc_gen_machine_store_to_stack_ex(R0, adj + 4, (1u << R1) | (1u << R2) | (1u << R3));
    tcc_gen_machine_store_to_stack_ex(R1, adj + 8, (1u << R0) | (1u << R2) | (1u << R3));
    tcc_gen_machine_store_to_stack_ex(R2, adj + 12, (1u << R0) | (1u << R1) | (1u << R3));
    tcc_gen_machine_store_to_stack_ex(R3, adj + 16, (1u << R0) | (1u << R1) | (1u << R2));
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

    /* Build a bitmask of ALL incoming argument registers (R0-R3) that need
     * to be saved.  When storing a spilled parameter to the stack at a large
     * offset, the scratch register allocator must NOT pick any of these
     * registers — they still hold incoming parameter values.
     *
     * Without this mask, storing e.g. R0 to [FP-1028] can use R1 as scratch
     * for the offset constant, destroying the parameter value in R1.
     */
    uint32_t incoming_arg_regs_mask = 0;
    for (int vreg = 0; vreg < ir->next_parameter; ++vreg)
    {
      const int encoded_vreg = (TCCIR_VREG_TYPE_PARAM << 28) | vreg;
      IRLiveInterval *interval = tcc_ir_get_live_interval(ir, encoded_vreg);
      if (!interval)
        continue;
      if (interval->incoming_reg0 >= 0 && interval->incoming_reg0 < 16)
        incoming_arg_regs_mask |= (1u << interval->incoming_reg0);
      if (interval->incoming_reg1 >= 0 && interval->incoming_reg1 < 16)
        incoming_arg_regs_mask |= (1u << interval->incoming_reg1);
    }

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
       * Use the incoming_arg_regs_mask to prevent the scratch allocator
       * from clobbering other incoming argument registers.
       */
      if (interval->allocation.offset != 0)
      {
        /* Adjust for callee-saved gap below FP in two-phase push. */
        const int stack_offset = fp_adjust_local_offset(interval->allocation.offset, 0);
        if (is_64bit && incoming_r1 >= 0)
        {
          tcc_gen_machine_store_to_stack_ex(incoming_r0, stack_offset, incoming_arg_regs_mask);
          /* R0 is now saved; remove it from the protection mask */
          incoming_arg_regs_mask &= ~(1u << incoming_r0);
          tcc_gen_machine_store_to_stack_ex(incoming_r1, stack_offset + 4, incoming_arg_regs_mask);
          incoming_arg_regs_mask &= ~(1u << incoming_r1);
        }
        else
        {
          tcc_gen_machine_store_to_stack_ex(incoming_r0, stack_offset, incoming_arg_regs_mask);
          incoming_arg_regs_mask &= ~(1u << incoming_r0);
        }
        continue;
      }

      /* Register-allocated parameters: record reg->reg moves (parallel move).
       *
       * IMPORTANT: Do NOT remove incoming_r0/r1 from incoming_arg_regs_mask
       * here!  The reg->reg moves are only COLLECTED now and executed LATER
       * as a parallel move.  The incoming register still holds the live
       * parameter value at this point.  If we removed it from the mask, a
       * subsequent spill-store for another parameter could use it as scratch
       * and destroy the value before the parallel move consumes it.
       */
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

        ot_check_mov_reg(dst, src, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
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
      ot_check_mov_reg(temp, start, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);

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
          ot_check_mov_reg(cur, temp, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
          break;
        }

        ot_check_mov_reg(cur, src, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
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

  if (tcc_state->need_frame_pointer && callee_saved_regs)
  {
    /* ── Two-phase pop (mirrors two-phase push) ── */
    /* Restore SP from FP (works even with alloca/VLA since FP is stable) */
    ot_check_mov_reg(R_SP, R_FP, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
    /* SP = FP; callee-saved regs are below FP. Adjust SP down.
     * R3 is free in the epilogue (not used for return values). */
    gadd_sp_ex(-callee_push_size, R3);
    ot_check(th_pop(callee_saved_regs));
    /* SP is now at FP (pointing at frame record {r7, [lr]}) */
    if (vararg_push_size > 0 && lr_saved)
    {
      /* Variadic: pop FP+LR, then skip over the pushed r0-r3 area */
      ot_check(th_pop((1 << R_FP) | (1 << R_LR)));
      gadd_sp_ex(vararg_push_size, R3);
      ot_check(th_bx_reg(R_LR));
    }
    else if (lr_saved)
    {
      ot_check(th_pop((1 << R_FP) | (1 << R_PC)));
    }
    else
    {
      ot_check(th_pop(1 << R_FP));
      if (vararg_push_size > 0)
        gadd_sp_ex(vararg_push_size, R3);
      ot_check(th_bx_reg(R_LR));
    }
  }
  else if (tcc_state->need_frame_pointer)
  {
    /* ── Original single-push with FP: restore SP from FP, then pop all ── */
    ot_check_mov_reg(R_SP, R_FP, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
    if (vararg_push_size > 0 && lr_saved)
    {
      /* Variadic: pop all regs with LR (not PC), then skip pushed r0-r3 */
      ot_check(th_pop(pushed_registers));
      gadd_sp_ex(vararg_push_size, R3);
      ot_check(th_bx_reg(R_LR));
    }
    else if (lr_saved)
    {
      pushed_registers |= 1 << R_PC;
      pushed_registers &= ~(1 << R_LR);
      ot_check(th_pop(pushed_registers));
    }
    else
    {
      if (pushed_registers > 0)
        ot_check(th_pop(pushed_registers));
      if (vararg_push_size > 0)
        gadd_sp_ex(vararg_push_size, R3);
      ot_check(th_bx_reg(R_LR));
    }
  }
  else
  {
    /* ── No frame pointer ── */
    if (epilogue_stack_dealloc > 0)
      gadd_sp_ex(epilogue_stack_dealloc, R3);
    if (lr_saved)
    {
      pushed_registers |= 1 << R_PC;
      pushed_registers &= ~(1 << R_LR);
      ot_check(th_pop(pushed_registers));
    }
    else
    {
      if (pushed_registers > 0)
        ot_check(th_pop(pushed_registers));
      ot_check(th_bx_reg(R_LR));
    }
  }

  thumb_gen_state.generating_function = 0;
  th_literal_pool_generate();
  thumb_free_call_sites();
}

ST_FUNC void tcc_gen_machine_finish_noreturn(void)
{
  thumb_gen_state.generating_function = 0;
  th_literal_pool_generate();
  thumb_free_call_sites();
}

/* Load Effective Address: compute the address of src1 into dest.
 * This is the explicit "address-of" operation for local variables/arrays.
 * Unlike LOAD which dereferences, LEA computes FP+offset into a register.
 */

/* MachineOperand-based LEA.  Computes the address of the source operand
 * into the destination.
 *
 * Most operand kinds are handled directly by mach_ensure_in_reg:
 *   MACH_OP_FRAME_ADDR  → ADD dest, FP, #offset  (local variable address)
 *   MACH_OP_SYMBOL      → LDR dest, =symbol      (global variable address)
 *   MACH_OP_REG         → MOV dest, src_reg       (address already computed)
 *
 * PARAM_STACK and CHAIN_REL need special handling because mach_ensure_in_reg
 * always loads the VALUE from the stack.  For LEA, we need the ADDRESS instead:
 *   MACH_OP_PARAM_STACK → ADD dest, FP, #(offset + offset_to_args)
 *   MACH_OP_CHAIN_REL   → ADD/SUB dest, chain_base, #offset
 */
ST_FUNC void tcc_gen_machine_lea_mop(MachineOperand dest, MachineOperand src)
{
  MachineCodegenContext ctx = {0};
  int r;

  /* When dest is a writable register, compute the address directly into it
   * to avoid a scratch + redundant mov. mach_alloc_scratch is driven by the
   * per-instruction live_regs bitmap which already marks the dest reg "live"
   * (it's the def target), so it would otherwise pick a different reg and
   * writeback would emit `mov dest_reg, scratch`. */
  int dest_reg = -1;
  if (dest.kind == MACH_OP_REG && !dest.needs_deref &&
      dest.u.reg.r0 != (int)PREG_REG_NONE)
    dest_reg = dest.u.reg.r0;

  switch (src.kind)
  {
  case MACH_OP_PARAM_STACK:
  {
    /* Compute address of caller's argument slot. */
    r = (dest_reg >= 0) ? dest_reg : mach_alloc_scratch(&ctx, 0);
    tcc_machine_addr_of_stack_slot(r, src.u.param.offset, 1 /* is_param */);
    break;
  }
  case MACH_OP_CHAIN_REL:
  {
    /* Compute address in parent frame via static chain. */
    ScratchRegAlloc chain_scratch = {0};
    int chain_used = 0;
    uint32_t excl = 0;
    int base = resolve_chain_base(tcc_state->ir, src.u.chain.chain_index, excl, &chain_scratch, &chain_used);
    /* dest_reg only usable if it doesn't collide with the chain base */
    if (dest_reg >= 0 && dest_reg != base && !(excl & (1u << (uint32_t)dest_reg)))
      r = dest_reg;
    else
      r = mach_alloc_scratch(&ctx, excl | (1u << (uint32_t)base));
    int32_t off = src.u.chain.offset;
    int sign = (off < 0);
    int abs_off = sign ? (int)(-off) : (int)off;
    if (abs_off == 0)
    {
      if (r != base)
        ot_check_mov_reg((uint32_t)r, (uint32_t)base, flags_safe(), THUMB_SHIFT_DEFAULT,
                         ENFORCE_ENCODING_NONE, false);
    }
    else
    {
      thumb_opcode ins = sign ? th_sub_imm(r, base, abs_off, flags_safe(), ENFORCE_ENCODING_NONE)
                              : th_add_imm(r, base, abs_off, flags_safe(), ENFORCE_ENCODING_NONE);
      if (ins.size != 0)
      {
        ot_check(ins);
      }
      else
      {
        /* Large offset: load into a scratch and use register ADD/SUB */
        ScratchRegAlloc off_sc = get_scratch_reg_with_save(excl | (1u << (uint32_t)r) | (1u << (uint32_t)base));
        load_full_const(off_sc.reg, PREG_NONE, LFC_SPLIT(abs_off));
        ot_check(sign ? th_sub_reg(r, base, off_sc.reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                                   ENFORCE_ENCODING_NONE)
                      : th_add_reg(r, base, off_sc.reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                                   ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&off_sc);
      }
    }
    if (chain_used)
      restore_scratch_reg(&chain_scratch);
    break;
  }
  case MACH_OP_FRAME_ADDR:
    /* Address of a local stack slot: ADD dest, sp, #offset. */
    r = (dest_reg >= 0) ? dest_reg : mach_alloc_scratch(&ctx, 0);
    tcc_machine_addr_of_stack_slot(r, src.u.frame.offset, 0);
    break;
  case MACH_OP_SYMBOL:
    if (!src.needs_deref)
    {
      Sym *raw_sym = src.u.sym.sym;
      Sym *sym = raw_sym ? validate_sym_for_reloc(raw_sym) : NULL;
      r = (dest_reg >= 0) ? dest_reg : mach_alloc_scratch(&ctx, 0);
      tcc_machine_load_constant(r, PREG_REG_NONE, src.u.sym.addend, 0, sym);
      break;
    }
    /* fallthrough for needs_deref */
  default:
    /* REG and other lvalue-y forms: mach_ensure_in_reg already computes the address. */
    r = mach_ensure_in_reg(&ctx, &src, 0);
    break;
  }

  mach_writeback_dest(&dest, r);
  mach_release_all(&ctx);
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
  tcc_gen_machine_store_to_stack_ex(reg, offset, 0);
}

/* Store a register to a FP-relative stack slot, with additional register
 * exclusions for the scratch allocator.  The extra_exclude mask prevents
 * the scratch register allocator from picking registers that still hold
 * live values (e.g. incoming argument registers during the prologue).
 */
ST_FUNC void tcc_gen_machine_store_to_stack_ex(int reg, int offset, uint32_t extra_exclude)
{
  const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
  int sign = (offset < 0);
  int abs_offset = sign ? -offset : offset;

  /* Try direct STR with immediate offset */
  if (!store_word_to_base(reg, base_reg, abs_offset, sign))
  {
    /* Offset too large, use scratch register */
    /* Don't reuse the source register as offset scratch, otherwise we'd
     * clobber the value before the STR (e.g. store -offset instead of value). */
    ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(abs_offset, sign, (1u << reg) | (1u << base_reg) | extra_exclude);
    int rr = rr_alloc.reg;
    ot_check(th_str_reg(reg, base_reg, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
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

/* MachineOperand variant of gcall_or_jump.
 * Called from func_call_mop after argument setup is complete.
 *
 * MACH_OP_SYMBOL  → direct call via BL + relocation
 * MACH_OP_IMM     → relative call (rare)
 * MACH_OP_REG     → indirect call: BLX through register
 * Other kinds     → load to scratch via mach_ensure_in_reg, then BLX
 *
 * For btype=FUNC + needs_deref: the register already holds the function
 * pointer value (not an address to load through), so needs_deref is cleared.
 */
static void gcall_or_jump_mop(int is_jmp, MachineOperand target)
{
  /* Tail-call: promote is_jmp so we emit B/BX instead of BL/BLX. */
  if (tail_call_pending)
    is_jmp = 1;

  if (target.kind == MACH_OP_SYMBOL)
  {
    /* Direct call via BL (or B.W for tail call) with relocation. */
    Sym *sym = target.u.sym.sym;
    int32_t addend = target.u.sym.addend;
    Sym *validated_sym = sym ? validate_sym_for_reloc(sym) : NULL;
    Sym *reloc_sym = NULL;

    if (!dry_run_state.active)
    {
      if (sym && !validated_sym && !(sym->v & SYM_FIELD))
      {
        put_extern_sym(sym, NULL, 0, 0);
        validated_sym = validate_sym_for_reloc(sym);
      }
      if (sym && !(sym->v & SYM_FIELD))
        reloc_sym = validated_sym ? validated_sym : sym;
    }

    uint32_t imm;
    if (reloc_sym)
      imm = (uint32_t)-4; /* placeholder for linker */
    else
      imm = th_encbranch(ind, ind + addend);

    TRACE("gcall_or_jmp_mop: %d, ind: 0x%x, 0x%x", is_jmp, ind, imm);
    if (imm)
    {
      if (is_jmp)
        ot_check(th_b_t4((int32_t)imm));
      else
        ot_check(th_bl_t1(imm));
      if (!dry_run_state.active && reloc_sym)
      {
        int call_pos = ind - 4;
        greloc(cur_text_section, reloc_sym, call_pos, R_ARM_THM_JUMP24);
      }
    }
    return;
  }

  if (target.kind == MACH_OP_IMM)
  {
    /* Relative call (rare). */
    uint32_t imm = th_encbranch(ind, ind + (int32_t)target.u.imm.val);
    TRACE("gcall_or_jmp_mop(imm): %d, ind: 0x%x, 0x%x", is_jmp, ind, imm);
    if (imm)
    {
      if (is_jmp)
        ot_check(th_b_t4((int32_t)imm));
      else
        ot_check(th_bl_t1(imm));
    }
    return;
  }

  /* Indirect call through register/spill/frame/param.
   *
   * For btype=FUNC with needs_deref: the register already holds the function
   * pointer value, not an address to dereference.  Clear needs_deref to avoid
   * a spurious LDR before BLX. */
  MachineOperand adjusted = target;
  if (adjusted.btype == IROP_BTYPE_FUNC && adjusted.needs_deref && adjusted.kind == MACH_OP_REG)
    adjusted.needs_deref = false;

  /* Keep R0-R3 safe during target materialization. */
  const uint32_t arg_regs = (1u << R0) | (1u << R1) | (1u << R2) | (1u << R3);
  MachineCodegenContext mctx = {0};

  if (is_jmp)
  {
    int r = mach_ensure_in_reg(&mctx, &adjusted, arg_regs);
    ot_check(th_bx_reg(r));
  }
  else
  {
    /* For calls, allocate scratch excluding R0-R3 so args are preserved. */
    uint32_t old_exclude = scratch_global_exclude;
    scratch_global_exclude |= arg_regs;

    int r = mach_ensure_in_reg(&mctx, &adjusted, arg_regs);

    scratch_global_exclude = old_exclude;
    ot_check(th_blx_reg(r));
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
    _lfc_sym = sym;
    load_full_const(reg, PREG_NONE, imm, 0);
    return;
  }

  /* Try to encode as ARM immediate (supports various rotated 8-bit patterns) */
  if (!ot(th_generic_mov_imm(reg, imm)))
  {
    /* Value doesn't fit in immediate encoding, use literal pool */
    load_full_const(reg, PREG_NONE, imm, 0);
  }
}

typedef enum ThumbArgMoveKind
{
  THUMB_ARG_MOVE_REG,
  THUMB_ARG_MOVE_IMM,
  THUMB_ARG_MOVE_IMM64,      /* load 64-bit immediate into register pair */
  THUMB_ARG_MOVE_LOCAL_ADDR, /* compute address of local: fp + offset */
  THUMB_ARG_MOVE_STRUCT,     /* load struct words into consecutive registers */
  THUMB_ARG_MOVE_MOP,        /* generic: load MachineOperand into dst_reg (+ dst_reg_hi for 64-bit) */
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
  int struct_word_count; /* valid when kind==THUMB_ARG_MOVE_STRUCT */
  int struct_src_align;  /* struct natural alignment (bytes); gates source LDRD */
  MachineOperand mop;    /* valid when kind==THUMB_ARG_MOVE_MOP */
} ThumbArgMove;

/* Context for function call generation - reduces parameter passing */
typedef struct CallGenContext
{
  ThumbGenCallSite *call_site;
  TCCAbiCallLayout *layout;
  IROperand *args;
  MachineOperand *mops;
  int argc;
  int stack_size;
  uint32_t arg_move_dst_mask; /* Registers that will be explicitly written by register arg moves.
                               * These are safe to clobber as scratch during stack arg placement
                               * because the subsequent register moves will overwrite them. */
} CallGenContext;

static void thumb_emit_arg_move(const ThumbArgMove *m)
{
  if (m->kind == THUMB_ARG_MOVE_REG)
  {
    if (m->src_reg == m->dst_reg)
      return;
    ot_check_mov_reg(m->dst_reg, m->src_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                     false);
    return;
  }

  if (m->kind == THUMB_ARG_MOVE_LOCAL_ADDR)
  {
    /* Compute address of local variable: dst = fp + offset */
    tcc_machine_addr_of_stack_slot(m->dst_reg, m->local_offset, m->local_is_param);
    return;
  }

  if (m->kind == THUMB_ARG_MOVE_STRUCT)
  {
    /* Load struct words into consecutive registers.
     * The mop contains the struct operand; get its base address. */
    int word_count = m->struct_word_count;
    int base_dst = m->dst_reg;

    /* LDRD fast path for the common 2-word (8-byte) aggregate case sourced
     * directly from a stack-backed location.  Mirrors the LDRD path used by
     * THUMB_ARG_MOVE_MOP for 64-bit scalars — Thumb-2 LDRD requires natural
     * 4-byte alignment, which spill slots and the caller param stack both
     * provide.  Skips the scratch + per-word loads that would otherwise
     * emit `add.w ip, sp, #N; ldr lo, [ip]; ldr hi, [ip, #4]` (3 insts).
     *
     * LDRD writes Rt before Rt2, so Rt2 (dst+1) must not equal the base
     * register, otherwise the 2nd half reads from a clobbered base. */
    if (word_count == 2 && !m->mop.needs_deref &&
        (m->mop.kind == MACH_OP_SPILL || m->mop.kind == MACH_OP_PARAM_STACK))
    {
      int raw_off =
          (m->mop.kind == MACH_OP_SPILL) ? m->mop.u.spill.offset : m->mop.u.param.offset + offset_to_args;
      int adjusted = (m->mop.kind == MACH_OP_SPILL) ? fp_adjust_local_offset(raw_off, 0) : raw_off;
      int ldrd_base = tcc_state->need_frame_pointer ? R_FP : R_SP;
      int ldrd_sign = (adjusted < 0);
      int ldrd_abs_off = ldrd_sign ? -adjusted : adjusted;
      int dst_hi = base_dst + 1;
      if (dst_hi != ldrd_base && base_dst != ldrd_base &&
          try_ldrd_pair(base_dst, dst_hi, ldrd_base, ldrd_abs_off, ldrd_sign))
      {
        return;
      }
    }

    /* Get the struct base address into a scratch register */
    ScratchRegAlloc struct_scratch = get_scratch_reg_with_save(0);
    int base_addr_reg = get_struct_base_addr_mop(&m->mop, struct_scratch.reg);

    /* Load each word from the struct into consecutive target registers.
     * Adjacent word pairs use LDRD when the struct's natural alignment is >= 4
     * (so the source address is 4-byte aligned — LDRD faults otherwise) and
     * neither destination register aliases the base (LDRD writes Rt then Rt2;
     * an alias would read a clobbered base on the fallback path / be unsafe). */
    bool src_aligned = (m->struct_src_align >= 4);
    int w = 0;
    for (; w + 1 < word_count; )
    {
      int dst = base_dst + w;
      int dst_hi = base_dst + w + 1;
      int offset = w * 4;
      if (src_aligned && dst != base_addr_reg && dst_hi != base_addr_reg &&
          tcc_gen_machine_try_ldrd_base(dst, dst_hi, base_addr_reg, offset))
      {
        w += 2;
        continue;
      }
      /* Single-word load of this word; the next iteration handles w+1. */
      if (!load_word_from_base(dst, base_addr_reg, offset, 0))
      {
        ScratchRegAlloc off_scratch = get_scratch_reg_with_save((1u << base_addr_reg) | (1u << dst));
        load_immediate(off_scratch.reg, offset, NULL, false);
        ot_check(th_ldr_reg(dst, base_addr_reg, off_scratch.reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&off_scratch);
      }
      w += 1;
    }
    /* Trailing odd word. */
    for (; w < word_count; ++w)
    {
      int dst = base_dst + w;
      int offset = w * 4;
      if (!load_word_from_base(dst, base_addr_reg, offset, 0))
      {
        ScratchRegAlloc off_scratch = get_scratch_reg_with_save((1u << base_addr_reg) | (1u << dst));
        load_immediate(off_scratch.reg, offset, NULL, false);
        ot_check(th_ldr_reg(dst, base_addr_reg, off_scratch.reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&off_scratch);
      }
    }
    restore_scratch_reg(&struct_scratch);
    return;
  }

  if (m->kind == THUMB_ARG_MOVE_MOP)
  {
    /* Generic MachineOperand → register load.
     * Handles all MOP kinds (REG+deref, SPILL, PARAM_STACK, CHAIN_REL,
     * SYMBOL+deref, etc.) via mach_ensure_in_reg. */
    MachineCodegenContext mctx = {0};
    if (m->mop.is_64bit && m->dst_reg_hi != 0 && m->dst_reg_hi != PREG_REG_NONE)
    {
      if (m->mop.needs_deref && m->mop.kind != MACH_OP_PARAM_STACK)
      {
        /* The operand holds a pointer (in reg, spill, etc.).  Load the
         * pointer into a register, then fetch lo/hi from [ptr+0]/[ptr+4].
         * mach_make_hi_half cannot handle this because it adjusts the
         * storage location (e.g. spill offset) instead of the deref offset.
         *
         * PARAM_STACK is excluded: mach_ensure_in_reg for PARAM_STACK
         * always loads directly from the caller's argument area, so the
         * mach_make_lo/hi_half path handles it correctly. */
        int base;
        if (m->mop.kind == MACH_OP_REG)
        {
          base = m->mop.u.reg.r0;
        }
        else
        {
          MachineOperand addr = m->mop;
          addr.needs_deref = false;
          addr.is_64bit = false;
          addr.btype = IROP_BTYPE_INT32;
          uint32_t excl = (1u << m->dst_reg) | (1u << m->dst_reg_hi);
          base = mach_ensure_in_reg(&mctx, &addr, excl);
        }
        /* Use the 64-bit load_from_base path so it preserves the base when
         * base == dst_reg (otherwise the lo-load would clobber it before
         * the hi-load can use it). */
        load_from_base(m->dst_reg, m->dst_reg_hi, IROP_BTYPE_INT64, 0, 0, 0, (uint32_t)base);
      }
      else
      {
        /* Fast path: when the source is a stack-backed 64-bit value (spill
         * slot or caller's param stack area) and the destination is a valid
         * AAPCS register pair, emit a single LDRD straight into dst_reg /
         * dst_reg_hi, skipping the scratch + MOV sequence that the generic
         * lo/hi lowering below would produce.
         *
         * Stack spill slots and the caller-argument frame are guaranteed
         * 8-byte aligned (AAPCS stack_align = 8, spill slots obey type
         * alignment), so LDRD's 4-byte alignment requirement is satisfied. */
        int ldrd_base = -1;
        int ldrd_abs_off = 0;
        int ldrd_sign = 0;
        int ldrd_ok = 0;
        if (!m->mop.needs_deref && (m->mop.kind == MACH_OP_SPILL || m->mop.kind == MACH_OP_PARAM_STACK))
        {
          int raw_off = (m->mop.kind == MACH_OP_SPILL) ? m->mop.u.spill.offset : m->mop.u.param.offset + offset_to_args;
          int adjusted = (m->mop.kind == MACH_OP_SPILL) ? fp_adjust_local_offset(raw_off, 0) : raw_off;
          ldrd_base = tcc_state->need_frame_pointer ? R_FP : R_SP;
          ldrd_sign = (adjusted < 0);
          ldrd_abs_off = ldrd_sign ? -adjusted : adjusted;
          ldrd_ok = 1;
        }
        if (ldrd_ok && try_ldrd_pair(m->dst_reg, m->dst_reg_hi, ldrd_base, ldrd_abs_off, ldrd_sign))
        {
          /* LDRD emitted. */
        }
        else
        {
          /* 64-bit: load lo and hi halves separately. */
          MachineOperand lo = mach_make_lo_half(&m->mop);
          MachineOperand hi = mach_make_hi_half(&m->mop);
          uint32_t excl = (1u << m->dst_reg) | (1u << m->dst_reg_hi);
          int r_lo = mach_ensure_in_reg(&mctx, &lo, excl);
          int r_hi = mach_ensure_in_reg(&mctx, &hi, excl | (1u << (uint32_t)r_lo));
          if (r_lo != m->dst_reg)
            ot_check_mov_reg(m->dst_reg, r_lo, flags_safe(), THUMB_SHIFT_DEFAULT,
                             ENFORCE_ENCODING_NONE, false);
          if (r_hi != m->dst_reg_hi)
            ot_check_mov_reg(m->dst_reg_hi, r_hi, flags_safe(), THUMB_SHIFT_DEFAULT,
                             ENFORCE_ENCODING_NONE, false);
        }
      }
    }
    else
    {
      /* 32-bit: prefer loading directly into dst_reg when the operand kind
       * permits it, bypassing the scratch + MOV sequence that
       * mach_ensure_in_reg would emit.  Kinds that need an extra
       * pointer-chain scratch beyond dst_reg (CHAIN_REL) fall through to
       * the generic path. */
      const MachineOperand *mop = &m->mop;
      const int dst = m->dst_reg;
      int handled = 0;

      switch (mop->kind)
      {
      case MACH_OP_NONE:
        tcc_machine_load_constant(dst, PREG_REG_NONE, 0, 0, NULL);
        handled = 1;
        break;

      case MACH_OP_REG:
        if (mop->needs_deref)
        {
          /* LDR dst, [r0]; legal even when r0 == dst (loaded value
           * just replaces the base). */
          load_from_base(dst, PREG_REG_NONE, mop->btype, (int)mop->is_unsigned, 0, 0,
                         (uint32_t)mop->u.reg.r0);
        }
        else if (mop->u.reg.r0 != dst)
        {
          ot_check_mov_reg(dst, mop->u.reg.r0, flags_safe(), THUMB_SHIFT_DEFAULT,
                           ENFORCE_ENCODING_NONE, false);
        }
        handled = 1;
        break;

      case MACH_OP_SPILL:
        if (!mop->needs_deref)
        {
          tcc_machine_load_spill_slot(dst, mop->u.spill.offset);
        }
        else
        {
          /* LLOCAL: load pointer into dst, then dereference into dst. */
          tcc_machine_load_spill_slot(dst, mop->u.spill.offset);
          load_from_base(dst, PREG_REG_NONE, mop->btype, (int)mop->is_unsigned, 0, 0,
                         (uint32_t)dst);
        }
        handled = 1;
        break;

      case MACH_OP_PARAM_STACK:
      {
        const int adjusted = mop->u.param.offset + offset_to_args;
        const int base_reg = tcc_state->need_frame_pointer ? R_FP : R_SP;
        const int sign = (adjusted < 0);
        const int abs_off = sign ? -adjusted : adjusted;
        load_from_base(dst, PREG_REG_NONE, mop->btype, (int)mop->is_unsigned, abs_off, sign,
                       (uint32_t)base_reg);
        handled = 1;
        break;
      }

      case MACH_OP_IMM:
        tcc_machine_load_constant(dst, PREG_REG_NONE, mop->u.imm.val, 0, NULL);
        handled = 1;
        break;

      case MACH_OP_FRAME_ADDR:
        if (!mop->needs_deref)
        {
          tcc_machine_addr_of_stack_slot(dst, mop->u.frame.offset, 0);
        }
        else
        {
          tcc_machine_addr_of_stack_slot(dst, mop->u.frame.offset, 0);
          load_from_base(dst, PREG_REG_NONE, mop->btype, (int)mop->is_unsigned, 0, 0,
                         (uint32_t)dst);
        }
        handled = 1;
        break;

      case MACH_OP_SYMBOL:
      {
        Sym *raw_sym = mop->u.sym.sym;
        Sym *sym = raw_sym ? validate_sym_for_reloc(raw_sym) : NULL;
        if (!mop->needs_deref)
        {
          tcc_machine_load_constant(dst, PREG_REG_NONE, mop->u.sym.addend, 0, sym);
        }
        else
        {
          tcc_machine_load_constant(dst, PREG_REG_NONE, 0, 0, sym);
          const int32_t addend = mop->u.sym.addend;
          const int sign = (addend < 0);
          const int abs_off = sign ? (int)(-addend) : (int)addend;
          load_from_base(dst, PREG_REG_NONE, mop->btype, (int)mop->is_unsigned, abs_off, sign,
                         (uint32_t)dst);
        }
        handled = 1;
        break;
      }

      default:
        /* CHAIN_REL etc.: fall through to generic scratch + MOV. */
        break;
      }

      if (!handled)
      {
        uint32_t excl = (1u << dst);
        int r = mach_ensure_in_reg(&mctx, mop, excl);
        if (r != dst)
          ot_check_mov_reg(dst, r, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                           false);
      }
    }
    mach_release_all(&mctx);
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

/* Compute the full set of destination registers written by an arg move.
 * Multi-register moves (IMM64, 64-bit MOP, STRUCT) write more than dst_reg.
 * The parallel move scheduler must check ALL written registers against
 * pending source registers to avoid clobbering. */
static uint32_t arg_move_write_set(const ThumbArgMove *m)
{
  uint32_t set = (1u << m->dst_reg);
  switch (m->kind)
  {
  case THUMB_ARG_MOVE_IMM64:
    set |= (1u << m->dst_reg_hi);
    break;
  case THUMB_ARG_MOVE_MOP:
    if (m->dst_reg_hi > 0 && m->dst_reg_hi < 16)
      set |= (1u << m->dst_reg_hi);
    break;
  case THUMB_ARG_MOVE_STRUCT:
    for (int w = 1; w < m->struct_word_count; w++)
      set |= (1u << (m->dst_reg + w));
    break;
  default:
    break;
  }
  return set;
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
      /* Check ALL destination registers of this move against pending sources.
       * Multi-reg writes (IMM64, 64-bit MOP, STRUCT) must not clobber any
       * register that a pending REG move still needs to read. */
      if ((src_set & arg_move_write_set(&moves[i])) == 0)
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
          exclude |= arg_move_write_set(&moves[i]);
          if (moves[i].kind == THUMB_ARG_MOVE_REG)
            exclude |= (1u << moves[i].src_reg);
        }
        /* Also exclude SP/PC. */
        exclude |= (1u << ARM_SP) | (1u << ARM_PC);
        tmp_alloc = get_scratch_reg_with_save(exclude);
        have_tmp = 1;
      }

      thumb_require_materialized_reg("thumb_emit_parallel_arg_moves", "tmp", tmp_alloc.reg);
      ot_check_mov_reg(tmp_alloc.reg, moves[cyc].src_reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                       ENFORCE_ENCODING_NONE, false);
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
    ScratchRegAlloc sc = get_scratch_reg_with_save((1u << src_reg));
    load_immediate(sc.reg, stack_offset, NULL, false);
    ot_check(th_str_reg(src_reg, ARM_SP, sc.reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&sc);
  }
}

/* Store a word to stack, preserving R0 if needed as scratch */
static void store_word_to_stack_safe(int src_reg, int stack_offset, int base_addr_reg)
{
  if (!store_word_to_base(src_reg, ARM_SP, stack_offset, 0))
  {
    ScratchRegAlloc sc = get_scratch_reg_with_save((1u << src_reg) | (1u << base_addr_reg));
    load_immediate(sc.reg, stack_offset, NULL, false);
    ot_check(th_str_reg(src_reg, ARM_SP, sc.reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&sc);
  }
}

/* Get struct base address into a register (MOP path).
 * For struct arguments, we want the ADDRESS of the struct, not a word from it.
 * The MOP from machine_op_from_ir encodes the "value-level" view, so we
 * convert / strip one level of indirection to obtain the address instead. */
static int get_struct_base_addr_mop(const MachineOperand *mop, int default_reg)
{
  switch (mop->kind)
  {
  case MACH_OP_REG:
    /* Register holds the struct address (for both needs_deref=true and false,
     * the register value IS the address we want for struct copying). */
    return mop->u.reg.r0;

  case MACH_OP_FRAME_ADDR:
    /* Address-of local struct: compute FP + offset. */
    tcc_machine_addr_of_stack_slot(default_reg, mop->u.frame.offset, 0);
    return default_reg;

  case MACH_OP_SPILL:
    if (mop->needs_deref)
    {
      /* llocal: spill slot holds pointer to struct. Load just the pointer. */
      tcc_machine_load_spill_slot(default_reg, mop->u.spill.offset);
    }
    else
    {
      /* Local struct on stack: compute address FP + offset. */
      tcc_machine_addr_of_stack_slot(default_reg, mop->u.spill.offset, 0);
    }
    return default_reg;

  case MACH_OP_PARAM_STACK:
    /* Struct in caller's argument area: compute address with param adjustment. */
    tcc_machine_addr_of_stack_slot(default_reg, mop->u.param.offset, 1 /* is_param */);
    return default_reg;

  case MACH_OP_SYMBOL:
  {
    Sym *sym = mop->u.sym.sym ? validate_sym_for_reloc(mop->u.sym.sym) : NULL;
    load_immediate(default_reg, (uint32_t)mop->u.sym.addend, sym, false);
    return default_reg;
  }

  default:
  {
    /* CHAIN_REL, etc: generic path with needs_deref stripped. */
    MachineOperand addr_mop = *mop;
    addr_mop.needs_deref = false;
    MachineCodegenContext mctx = {0};
    int r = mach_ensure_in_reg(&mctx, &addr_mop, 0);
    mach_release_all(&mctx);
    return r;
  }
  }
}

/* Build register move for a struct argument (MOP path) */
static int build_reg_move_struct(ThumbArgMove *moves, int move_count, const MachineOperand *mop,
                                 const TCCAbiArgLoc *loc, int base_reg, ThumbGenCallSite *call_site,
                                 int src_align)
{
  int words = loc->reg_count;
  if (words > 0 && words <= 4)
  {
    moves[move_count++] = (ThumbArgMove){
        .kind = THUMB_ARG_MOVE_STRUCT,
        .dst_reg = base_reg,
        .mop = *mop,
        .struct_word_count = words,
        .struct_src_align = src_align,
    };
  }
  for (int w = 0; w < words && w < loc->reg_count; w++)
    call_site->registers_map |= (1 << (base_reg + w));
  return move_count;
}

/* Build register move for a 64-bit argument (MOP path) */
static int build_reg_move_64bit(ThumbArgMove *moves, int move_count, const MachineOperand *mop, const IROperand *arg,
                                int base_reg, ThumbGenCallSite *call_site, TCCIRState *ir)
{
  if (mop->kind == MACH_OP_REG && !mop->needs_deref && thumb_is_hw_reg(mop->u.reg.r0) && thumb_is_hw_reg(mop->u.reg.r1))
  {
    /* Both halves in registers — emit up to two REG moves. */
    if (mop->u.reg.r0 != base_reg)
      moves[move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_REG, .dst_reg = base_reg, .src_reg = mop->u.reg.r0};
    if (mop->u.reg.r1 != (base_reg + 1))
      moves[move_count++] =
          (ThumbArgMove){.kind = THUMB_ARG_MOVE_REG, .dst_reg = base_reg + 1, .src_reg = mop->u.reg.r1};
  }
  else if (mop->kind == MACH_OP_IMM)
  {
    const uint64_t imm64 = (uint64_t)mop->u.imm.val;
    moves[move_count++] =
        (ThumbArgMove){.kind = THUMB_ARG_MOVE_IMM64, .dst_reg = base_reg, .dst_reg_hi = base_reg + 1, .imm64 = imm64};
  }
  else
  {
    /* Generic: load MOP value into register pair at emit time.
     * Covers SPILL, PARAM_STACK, CHAIN_REL, REG+needs_deref, SYMBOL, etc. */
    MachineOperand m = *mop;
    m.is_64bit = true;
    moves[move_count++] =
        (ThumbArgMove){.kind = THUMB_ARG_MOVE_MOP, .dst_reg = base_reg, .dst_reg_hi = base_reg + 1, .mop = m};
  }

  call_site->registers_map |= (1 << base_reg) | (1 << (base_reg + 1));
  return move_count;
}

/* Build register move for a 32-bit argument (MOP path) */
static int build_reg_move_32bit(ThumbArgMove *moves, int move_count, const MachineOperand *mop, const IROperand *arg,
                                int base_reg, ThumbGenCallSite *call_site, TCCIRState *ir)
{
  switch (mop->kind)
  {
  case MACH_OP_REG:
    if (mop->needs_deref)
    {
      /* Register-indirect: needs dereference at emit time. */
      moves[move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_MOP, .dst_reg = base_reg, .mop = *mop};
    }
    else if (mop->u.reg.r0 != base_reg)
    {
      moves[move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_REG, .dst_reg = base_reg, .src_reg = mop->u.reg.r0};
    }
    break;

  case MACH_OP_IMM:
    moves[move_count++] =
        (ThumbArgMove){.kind = THUMB_ARG_MOVE_IMM, .dst_reg = base_reg, .imm = (uint32_t)mop->u.imm.val, .sym = NULL};
    break;

  case MACH_OP_SYMBOL:
    if (mop->needs_deref)
    {
      /* Load value from global symbol — emit at emit time. */
      moves[move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_MOP, .dst_reg = base_reg, .mop = *mop};
    }
    else
    {
      /* Load symbol address (with addend). */
      moves[move_count++] = (ThumbArgMove){
          .kind = THUMB_ARG_MOVE_IMM, .dst_reg = base_reg, .imm = (uint32_t)mop->u.sym.addend, .sym = mop->u.sym.sym};
    }
    break;

  case MACH_OP_FRAME_ADDR:
    moves[move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_LOCAL_ADDR,
                                         .dst_reg = base_reg,
                                         .local_offset = mop->u.frame.offset,
                                         .local_is_param = 0};
    break;

  default:
    /* SPILL, PARAM_STACK, CHAIN_REL, etc.: generic MOP load at emit time. */
    moves[move_count++] = (ThumbArgMove){.kind = THUMB_ARG_MOVE_MOP, .dst_reg = base_reg, .mop = *mop};
    break;
  }

  call_site->registers_map |= (1 << base_reg);
  return move_count;
}

/* Place a struct argument on stack (MOP path) */
/* Load one struct word at [base_addr_reg + off] into `reg`, falling back to a
 * register-offset load when `off` exceeds the LDR immediate range. */
static void load_struct_word_into(int reg, int base_addr_reg, int off)
{
  if (!load_word_from_base(reg, base_addr_reg, off, 0))
  {
    load_immediate(reg, off, NULL, false);
    ot_check(th_ldr_reg(reg, base_addr_reg, reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
  }
}

/* Copy a (possibly split) struct argument's stack portion into the outgoing
 * argument area.  `src_align` is the struct's natural alignment in bytes.
 *
 * Adjacent word pairs are copied with LDRD/STRD instead of two LDR/STR.  The
 * destination is the outgoing arg area — SP-relative with a word-multiple
 * offset and SP 8-byte aligned at the call boundary — so STRD is always
 * alignment-safe.  LDRD additionally requires the *source* address to be
 * 4-byte aligned, which holds exactly when the struct's natural alignment is
 * >= 4 (the stack portion starts at base + words_in_regs*4, a word multiple). */
static void place_stack_arg_struct(const MachineOperand *mop, const TCCAbiArgLoc *loc, int stack_offset,
                                   int src_align)
{
  int words_in_regs = (loc->kind == TCC_ABI_LOC_REG_STACK) ? loc->reg_count : 0;
  int struct_src_offset = words_in_regs * 4;
  int struct_size = (loc->kind == TCC_ABI_LOC_REG_STACK) ? loc->stack_size : loc->size;
  int words = (struct_size + 3) / 4;

  ScratchRegAlloc struct_sc = get_scratch_reg_with_save(0);
  int base_addr_reg = get_struct_base_addr_mop(mop, struct_sc.reg);

  /* Second data register (besides LR) for paired LDRD/STRD.  find_call_scratch
   * never pushes (SP-relative store offsets must stay valid) and we exclude LR
   * and the struct base; an R_IP last-resort result is a permanent scratch and
   * safe to clobber. */
  int data2 = find_call_scratch((1u << ARM_LR) | (1u << (uint32_t)base_addr_reg), 0);
  bool can_pair = (words >= 2 && data2 != ARM_LR && data2 != base_addr_reg && data2 >= 0 &&
                   data2 <= R_LR && data2 != R_SP);
  bool src_aligned = (src_align >= 4);

  int w = 0;
  if (can_pair)
  {
    for (; w + 1 < words; w += 2)
    {
      int src_off = struct_src_offset + w * 4;
      int dst_off = stack_offset + w * 4;

      if (!(src_aligned && tcc_gen_machine_try_ldrd_base(ARM_LR, data2, base_addr_reg, src_off)))
      {
        load_struct_word_into(ARM_LR, base_addr_reg, src_off);
        load_struct_word_into(data2, base_addr_reg, src_off + 4);
      }
      if (!tcc_gen_machine_try_strd_base(ARM_LR, data2, ARM_SP, dst_off))
      {
        store_word_to_stack_safe(ARM_LR, dst_off, base_addr_reg);
        store_word_to_stack_safe(data2, dst_off + 4, base_addr_reg);
      }
    }
  }

  /* Trailing odd word, or every word when pairing was unavailable. */
  for (; w < words; ++w)
  {
    int src_off = struct_src_offset + w * 4;
    int dst_off = stack_offset + w * 4;
    load_struct_word_into(ARM_LR, base_addr_reg, src_off);
    store_word_to_stack_safe(ARM_LR, dst_off, base_addr_reg);
  }
  restore_scratch_reg(&struct_sc);
}

/* Find a free scratch register via liveness (no push/pop).
 * Returns the register number, or R_IP as last resort.
 * Must not push/pop since SP-relative offsets for stack args would shift.
 *
 * Unlike tcc_ls_find_free_scratch_reg (which refuses callee-saved regs),
 * this also considers callee-saved registers already pushed in the prologue.
 * Those are safe to clobber because the epilogue will restore them.
 *
 * arg_move_dst_mask: registers that will be explicitly written by register
 * arg moves AFTER stack arg placement.  These are safe to clobber even if
 * currently live, because the subsequent moves will overwrite them.
 * Pass 0 when not in a pre-move stack arg placement context. */
static int find_call_scratch(uint32_t extra_exclude, uint32_t arg_move_dst_mask)
{
  TCCIRState *ir = tcc_state->ir;
  uint32_t exclude = scratch_global_exclude | extra_exclude;
  if (ir)
  {
    /* Standard path: try caller-saved regs via liveness */
    int reg = tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, exclude, ir->leaffunc);
    if (reg != PREG_NONE && reg >= 0 && reg < 16 && reg != R_SP && reg != R_PC)
      return reg;

    /* Extended path: try callee-saved regs that are already pushed in prologue
     * AND not live at this instruction (so we won't clobber active values). */
    if (ir->ls.live_regs_by_instruction && ir->codegen_instruction_idx >= 0 &&
        ir->codegen_instruction_idx < ir->ls.live_regs_by_instruction_size)
    {
      uint32_t live = ir->ls.live_regs_by_instruction[ir->codegen_instruction_idx];
      uint32_t callee_pushed = pushed_registers & 0x0FF0u; /* R4-R11 that were pushed */
      uint32_t candidates = callee_pushed & ~live & ~exclude;
      if (candidates)
      {
        /* Prefer low registers (R4-R7) for 16-bit encoding */
        int r = (int)__builtin_ctz(candidates);
        return r;
      }
    }

    /* Pre-move path: registers that are destinations of explicit (non-identity)
     * register arg moves can be used as scratch — the moves will overwrite them.
     * Prefer low registers for 16-bit encoding. */
    if (arg_move_dst_mask)
    {
      uint32_t candidates = arg_move_dst_mask & ~exclude;
      if (candidates)
      {
        int r = (int)__builtin_ctz(candidates);
        if (r >= 0 && r < 16 && r != R_SP && r != R_PC)
          return r;
      }
    }
  }
  return R_IP;
}

/* Place a 64-bit argument on stack (MOP path) */
static void place_stack_arg_64bit(const MachineOperand *mop, int stack_offset, TCCIRState *ir,
                                  uint32_t arg_move_dst_mask)
{
  int lo_offset = stack_offset;
  int hi_offset = stack_offset + 4;

  if (mop->kind == MACH_OP_REG && !mop->needs_deref && thumb_is_hw_reg(mop->u.reg.r0) && thumb_is_hw_reg(mop->u.reg.r1))
  {
    /* If either register is R0-R3, the value was already stored by
     * presave_stack_args_from_arg_regs before the register shuffle. */
    if (mop->u.reg.r0 <= ARM_R3 || mop->u.reg.r1 <= ARM_R3)
      return;
    store_word_to_stack(mop->u.reg.r0, lo_offset);
    store_word_to_stack(mop->u.reg.r1, hi_offset);
  }
  else if (mop->kind == MACH_OP_IMM)
  {
    uint64_t imm64 = (uint64_t)mop->u.imm.val;
    int scr = find_call_scratch(0, arg_move_dst_mask);
    load_immediate(scr, (uint32_t)imm64, NULL, false);
    store_word_to_stack(scr, lo_offset);
    load_immediate(scr, (uint32_t)(imm64 >> 32), NULL, false);
    store_word_to_stack(scr, hi_offset);
  }
  else if (mop->needs_deref && mop->kind != MACH_OP_PARAM_STACK)
  {
    /* The operand holds a pointer (in reg, spill, etc.), not the 64-bit
     * value itself.  Load the pointer into a register, then fetch the
     * lo/hi halves from [ptr+0] and [ptr+4].  Splitting via
     * mach_make_hi_half would incorrectly adjust the storage location
     * (e.g. spill offset) instead of the dereference offset.
     *
     * PARAM_STACK is excluded: mach_ensure_in_reg for PARAM_STACK always
     * loads directly from the caller's argument area (ignores needs_deref),
     * so the else path with mach_make_lo/hi_half handles it correctly.
     *
     * The base register must NOT be the scratch because both halves are
     * loaded into the scratch.  If base == scratch the first load would
     * clobber the pointer before the second load can use it. */
    int scr = find_call_scratch(0, arg_move_dst_mask);
    int base;
    MachineCodegenContext mctx = {0};
    bool need_release = false;
    if (mop->kind == MACH_OP_REG && mop->u.reg.r0 != scr)
    {
      base = mop->u.reg.r0;
    }
    else
    {
      MachineOperand addr = *mop;
      addr.needs_deref = false;
      addr.is_64bit = false;
      addr.btype = IROP_BTYPE_INT32;
      base = mach_ensure_in_reg(&mctx, &addr, (1u << scr));
      need_release = true;
    }
    load_from_base(scr, PREG_REG_NONE, IROP_BTYPE_INT32, 0, 0, 0, (uint32_t)base);
    store_word_to_stack(scr, lo_offset);
    load_from_base(scr, PREG_REG_NONE, IROP_BTYPE_INT32, 0, 4, 0, (uint32_t)base);
    store_word_to_stack(scr, hi_offset);
    if (need_release)
      mach_release_all(&mctx);
  }
  else
  {
    /* Load each 32-bit half individually.  Override btype to INT32 so that
     * mach_ensure_in_reg → load_from_base does a single-word LDR instead
     * of a 64-bit pair load (which would allocate an extra scratch via push,
     * shift SP, and corrupt the SP-relative store offsets below). */
    MachineOperand lo = mach_make_lo_half(mop);
    MachineOperand hi = mach_make_hi_half(mop);
    lo.btype = IROP_BTYPE_INT32;
    hi.btype = IROP_BTYPE_INT32;
    MachineCodegenContext mctx = {0};
    int r_lo = mach_ensure_in_reg(&mctx, &lo, 0);
    store_word_to_stack_safe(r_lo, lo_offset, r_lo);
    mach_release_all(&mctx);
    mctx = (MachineCodegenContext){0};
    int r_hi = mach_ensure_in_reg(&mctx, &hi, 0);
    store_word_to_stack_safe(r_hi, hi_offset, r_hi);
    mach_release_all(&mctx);
  }
}

/* Place a 32-bit argument on stack (MOP path) */
static void place_stack_arg_32bit(const MachineOperand *mop, int stack_offset, CallGenContext *ctx)
{
  switch (mop->kind)
  {
  case MACH_OP_REG:
    if (!mop->needs_deref)
    {
      /* Skip R0-R3 sources — handled in pre-shuffle save. */
      if (mop->u.reg.r0 <= ARM_R3)
        return;
      store_word_to_stack(mop->u.reg.r0, stack_offset);
    }
    else
    {
      /* Register-indirect: load through the register, then store to stack.
       * Must use btype-aware load so that byte/short values are properly
       * zero/sign-extended (LDRB/LDRH) instead of always doing a word LDR. */
      int scr = find_call_scratch(1u << mop->u.reg.r0, ctx->arg_move_dst_mask);
      load_from_base(scr, PREG_REG_NONE, mop->btype, mop->is_unsigned, 0, 0, mop->u.reg.r0);
      store_word_to_stack(scr, stack_offset);
    }
    break;

  case MACH_OP_IMM:
  {
    int scr = find_call_scratch(0, ctx->arg_move_dst_mask);
    load_immediate(scr, (uint32_t)mop->u.imm.val, NULL, false);
    store_word_to_stack(scr, stack_offset);
    break;
  }

  case MACH_OP_SYMBOL:
  {
    int scr = find_call_scratch(0, ctx->arg_move_dst_mask);
    Sym *sym = mop->u.sym.sym ? validate_sym_for_reloc(mop->u.sym.sym) : NULL;
    if (mop->needs_deref)
    {
      /* Load value from global symbol address. */
      load_immediate(scr, 0, sym, false);
      int32_t addend = mop->u.sym.addend;
      int sign = (addend < 0);
      int abs_off = sign ? -addend : addend;
      load_from_base(scr, PREG_REG_NONE, mop->btype, mop->is_unsigned, abs_off, sign, scr);
    }
    else
    {
      load_immediate(scr, (uint32_t)mop->u.sym.addend, sym, false);
    }
    store_word_to_stack(scr, stack_offset);
    break;
  }

  default:
  {
    /* SPILL, PARAM_STACK, FRAME_ADDR, CHAIN_REL: generic MOP load. */
    MachineCodegenContext mctx = {0};
    int r = mach_ensure_in_reg(&mctx, mop, 0);
    store_word_to_stack(r, stack_offset);
    mach_release_all(&mctx);
    break;
  }
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
    const MachineOperand *mop = &ctx->mops[i];
    const int bt = irop_get_btype(*arg);
    const int is_64bit = mop->is_64bit;

    if (loc->kind != TCC_ABI_LOC_REG && loc->kind != TCC_ABI_LOC_REG_STACK)
      continue;

    int base_reg = ARM_R0 + loc->reg_base;

    if (bt == IROP_BTYPE_STRUCT || arg->is_complex)
    {
      /* Complex values already in a register pair hold the actual value,
       * not a pointer to it.  Route through individual register moves
       * instead of the struct-copy path (which dereferences as an address). */
      if (arg->is_complex && mop->kind == MACH_OP_REG && !mop->needs_deref && mop->is_64bit)
      {
        int words = loc->reg_count;
        if (words >= 1 && mop->u.reg.r0 != base_reg)
          reg_moves[move_count++] =
              (ThumbArgMove){.kind = THUMB_ARG_MOVE_REG, .dst_reg = base_reg, .src_reg = mop->u.reg.r0};
        if (words >= 2 && mop->u.reg.r1 != (base_reg + 1))
          reg_moves[move_count++] =
              (ThumbArgMove){.kind = THUMB_ARG_MOVE_REG, .dst_reg = base_reg + 1, .src_reg = mop->u.reg.r1};
        for (int w = 0; w < words; w++)
          ctx->call_site->registers_map |= (1 << (base_reg + w));
      }
      else if (arg->is_complex && mop->kind == MACH_OP_IMM)
      {
        /* Complex immediate: split 64-bit packed value (real_lo | imag_hi)
         * into individual 32-bit register moves. */
        const uint64_t imm64 = (uint64_t)mop->u.imm.val;
        int words = loc->reg_count;
        if (words >= 1)
          reg_moves[move_count++] =
              (ThumbArgMove){.kind = THUMB_ARG_MOVE_IMM, .dst_reg = base_reg, .imm = (uint32_t)imm64, .sym = NULL};
        if (words >= 2)
          reg_moves[move_count++] = (ThumbArgMove){
              .kind = THUMB_ARG_MOVE_IMM, .dst_reg = base_reg + 1, .imm = (uint32_t)(imm64 >> 32), .sym = NULL};
        for (int w = 0; w < words; w++)
          ctx->call_site->registers_map |= (1 << (base_reg + w));
      }
      else
      {
        int src_align = 0;
        irop_type_size_align(*arg, &src_align);
        move_count = build_reg_move_struct(reg_moves, move_count, mop, loc, base_reg, ctx->call_site, src_align);
      }
    }
    else if (is_64bit)
    {
      if (loc->reg_count < 2)
        tcc_error("compiler_error: 64-bit register argument has insufficient registers");
      move_count = build_reg_move_64bit(reg_moves, move_count, mop, arg, base_reg, ctx->call_site, tcc_state->ir);
    }
    else
    {
      move_count = build_reg_move_32bit(reg_moves, move_count, mop, arg, base_reg, ctx->call_site, tcc_state->ir);
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
    const MachineOperand *mop = &ctx->mops[i];
    const int bt = mop->btype;

    if (loc->kind == TCC_ABI_LOC_REG)
      continue;
    if (bt == IROP_BTYPE_STRUCT || mop->is_complex)
      continue;
    if (mop->kind != MACH_OP_REG || mop->needs_deref)
      continue;

    if (mop->is_64bit)
    {
      /* Pre-save 64-bit register pair if either register is in R0-R3.
       * The register arg shuffle will overwrite R0-R3, so both halves
       * must be stored to the stack before that happens. */
      int r0 = mop->u.reg.r0;
      int r1 = mop->u.reg.r1;
      if ((thumb_is_hw_reg(r0) && r0 <= ARM_R3) || (thumb_is_hw_reg(r1) && r1 <= ARM_R3))
      {
        int stack_offset = loc->stack_off;
        if (thumb_is_hw_reg(r0))
          store_word_to_stack(r0, stack_offset);
        if (thumb_is_hw_reg(r1))
          store_word_to_stack(r1, stack_offset + 4);
      }
    }
    else
    {
      /* Only pre-save if operand is in R0-R3 (arg registers that get overwritten). */
      if (mop->u.reg.r0 <= ARM_R3)
      {
        store_word_to_stack(mop->u.reg.r0, loc->stack_off);
      }
    }
  }
}

/* True for a plain 32-bit immediate argument destined for a stack slot. */
static int is_simple_imm_stack_arg(const TCCAbiArgLoc *loc, const MachineOperand *mop)
{
  return loc->kind != TCC_ABI_LOC_REG && mop->kind == MACH_OP_IMM && !mop->is_64bit &&
         mop->btype != IROP_BTYPE_STRUCT && !mop->is_complex;
}

/* One collected immediate stack store, for the grouped/windowed emission path. */
typedef struct StackImmArg
{
  int off;
  uint32_t val;
} StackImmArg;

/* Order by 4 KB window, then value, then offset.  Grouping equal values within a
 * window lets each distinct value be materialized once per window instead of once
 * per argument; the window ordering bounds base-register re-materialization. */
static int stack_imm_arg_cmp(const void *a, const void *b)
{
  const StackImmArg *x = (const StackImmArg *)a;
  const StackImmArg *y = (const StackImmArg *)b;
  int wx = x->off & ~0xFFF, wy = y->off & ~0xFFF;
  if (wx != wy)
    return wx < wy ? -1 : 1;
  if (x->val != y->val)
    return x->val < y->val ? -1 : 1;
  if (x->off != y->off)
    return x->off < y->off ? -1 : 1;
  return 0;
}

/* Emit a single non-simple-immediate stack argument (struct/complex/64-bit, or a
 * non-immediate 32-bit source).  Extracted from place_stack_arguments so both the
 * inline and the grouped emission paths share identical handling. */
static void place_one_stack_arg(CallGenContext *ctx, const TCCAbiArgLoc *loc, const MachineOperand *mop,
                                int stack_offset, int arg_index)
{
  if (mop->btype == IROP_BTYPE_STRUCT || mop->is_complex)
  {
    /* Complex values in a register pair: store the stack portion directly
     * from registers instead of treating the pair as a memory pointer. */
    if (mop->is_complex && mop->kind == MACH_OP_REG && !mop->needs_deref && mop->is_64bit)
    {
      int words_in_regs = (loc->kind == TCC_ABI_LOC_REG_STACK) ? loc->reg_count : 0;
      int stack_bytes = (loc->kind == TCC_ABI_LOC_REG_STACK) ? loc->stack_size : loc->size;
      int stack_words = (stack_bytes + 3) / 4;
      int pair_regs[2] = {mop->u.reg.r0, mop->u.reg.r1};
      for (int w = 0; w < stack_words; w++)
      {
        int reg_idx = words_in_regs + w;
        if (reg_idx < 2)
          store_word_to_stack(pair_regs[reg_idx], stack_offset + w * 4);
      }
    }
    else if (mop->is_complex && mop->kind == MACH_OP_IMM)
    {
      /* Complex immediate on stack: split 64-bit packed value into words. */
      const uint64_t imm64 = (uint64_t)mop->u.imm.val;
      int words_in_regs = (loc->kind == TCC_ABI_LOC_REG_STACK) ? loc->reg_count : 0;
      int stack_bytes = (loc->kind == TCC_ABI_LOC_REG_STACK) ? loc->stack_size : loc->size;
      int stack_words = (stack_bytes + 3) / 4;
      int scr = find_call_scratch(0, ctx->arg_move_dst_mask);
      for (int w = 0; w < stack_words; w++)
      {
        int word_idx = words_in_regs + w;
        uint32_t word_val = (uint32_t)(imm64 >> (word_idx * 32));
        load_immediate(scr, word_val, NULL, false);
        store_word_to_stack(scr, stack_offset + w * 4);
      }
    }
    else
    {
      /* Struct's natural alignment gates source-side LDRD (see
       * place_stack_arg_struct).  Default conservatively to 1 (no LDRD) when
       * the originating IR operand is unavailable. */
      int src_align = 1;
      if (ctx->args && arg_index >= 0 && arg_index < ctx->argc)
      {
        int a = 0;
        irop_type_size_align(ctx->args[arg_index], &a);
        if (a > 0)
          src_align = a;
      }
      place_stack_arg_struct(mop, loc, stack_offset, src_align);
    }
  }
  else if (mop->is_64bit)
    place_stack_arg_64bit(mop, stack_offset, tcc_state->ir, ctx->arg_move_dst_mask);
  else
    place_stack_arg_32bit(mop, stack_offset, ctx);
}

/* Inline (original-order) emission of every stack argument.  Used for the common
 * case where stack args stay within the immediate-offset store range. */
static void place_stack_arguments_inline(CallGenContext *ctx)
{
  int cached_imm_reg = -1;
  uint32_t cached_imm_val = 0;

  for (int i = 0; i < ctx->argc; ++i)
  {
    const TCCAbiArgLoc *loc = &ctx->layout->locs[i];
    const MachineOperand *mop = &ctx->mops[i];

    if (loc->kind == TCC_ABI_LOC_REG)
      continue;

    int stack_offset = loc->stack_off;

    if (is_simple_imm_stack_arg(loc, mop))
    {
      uint32_t val = (uint32_t)mop->u.imm.val;
      int scr = find_call_scratch(0, ctx->arg_move_dst_mask);
      if (cached_imm_reg != scr || cached_imm_val != val)
      {
        load_immediate(scr, val, NULL, false);
        cached_imm_reg = scr;
        cached_imm_val = val;
      }
      store_word_to_stack(scr, stack_offset);
      continue;
    }

    cached_imm_reg = -1;
    place_one_stack_arg(ctx, loc, mop, stack_offset, i);
  }
}

/* Place all stack arguments.
 *
 * For the common case the inline path is byte-identical to before.  When simple
 * 32-bit immediate stack args spill beyond the immediate-offset store range
 * (offset > 4092) — exactly where the naive path emits movw+indexed (3 instr/arg)
 * — a windowed/grouped path is used instead:
 *   - a base register holds sp+window so each store is a single str.w [rb,#disp]
 *     (re-materialized only when crossing a 4 KB window, ~once / 1024 stores);
 *   - the immediate stores are reordered by (window, value) so each distinct
 *     value is loaded once per window rather than once per argument.
 * Reordering pure-immediate stores to distinct, non-aliasing stack slots leaves
 * the pre-call stack image unchanged, so it is observationally identical. */
static void place_stack_arguments(CallGenContext *ctx)
{
  int max_imm_off = -1;
  int imm_count = 0;
  for (int i = 0; i < ctx->argc; ++i)
  {
    const TCCAbiArgLoc *loc = &ctx->layout->locs[i];
    const MachineOperand *mop = &ctx->mops[i];
    if (is_simple_imm_stack_arg(loc, mop))
    {
      imm_count++;
      if (loc->stack_off > max_imm_off)
        max_imm_off = loc->stack_off;
    }
  }

  if (!(max_imm_off > 4092 && imm_count >= 2) || getenv("TCC_NO_STACK_ARG_GROUP"))
  {
    place_stack_arguments_inline(ctx);
    return;
  }

  /* --- Windowed/grouped path --- */

  /* Pass 1: emit every non-simple-immediate stack arg first, in original order. */
  for (int i = 0; i < ctx->argc; ++i)
  {
    const TCCAbiArgLoc *loc = &ctx->layout->locs[i];
    const MachineOperand *mop = &ctx->mops[i];
    if (loc->kind == TCC_ABI_LOC_REG || is_simple_imm_stack_arg(loc, mop))
      continue;
    place_one_stack_arg(ctx, loc, mop, loc->stack_off, i);
  }

  /* Reserve two stable scratch registers: rv (holds the value) and rb (base
   * address).  Both are free across the whole argument-setup region — the call's
   * register args are moved in afterwards, and find_call_scratch only returns
   * registers that are dead here or are arg-move destinations (overwritten
   * later).  Prefer the lower-numbered register for rv so value materialization
   * can use the 16-bit MOVS encoding. */
  int s0 = find_call_scratch(0, ctx->arg_move_dst_mask);
  int s1 = find_call_scratch(1u << s0, ctx->arg_move_dst_mask);
  if (s1 < s0)
  {
    int t = s0;
    s0 = s1;
    s1 = t;
  }
  int rv = s0, rb = s1;
  int regs_ok = (rv != rb && rv >= 0 && rv < 16 && rb >= 0 && rb < 16 && rv != ARM_SP && rv != ARM_PC &&
                 rb != ARM_SP && rb != ARM_PC);

  StackImmArg *items = regs_ok ? tcc_malloc(sizeof(StackImmArg) * imm_count) : NULL;
  if (!items)
  {
    /* Out of stable registers (or alloc failure): emit the immediate args inline. */
    int cached_imm_reg = -1;
    uint32_t cached_imm_val = 0;
    for (int i = 0; i < ctx->argc; ++i)
    {
      const TCCAbiArgLoc *loc = &ctx->layout->locs[i];
      const MachineOperand *mop = &ctx->mops[i];
      if (!is_simple_imm_stack_arg(loc, mop))
        continue;
      uint32_t val = (uint32_t)mop->u.imm.val;
      int scr = find_call_scratch(0, ctx->arg_move_dst_mask);
      if (cached_imm_reg != scr || cached_imm_val != val)
      {
        load_immediate(scr, val, NULL, false);
        cached_imm_reg = scr;
        cached_imm_val = val;
      }
      store_word_to_stack(scr, loc->stack_off);
    }
    return;
  }

  int n = 0;
  for (int i = 0; i < ctx->argc; ++i)
  {
    const TCCAbiArgLoc *loc = &ctx->layout->locs[i];
    const MachineOperand *mop = &ctx->mops[i];
    if (!is_simple_imm_stack_arg(loc, mop))
      continue;
    items[n].off = loc->stack_off;
    items[n].val = (uint32_t)mop->u.imm.val;
    n++;
  }
  qsort(items, n, sizeof(StackImmArg), stack_imm_arg_cmp);

  uint32_t saved_excl = scratch_global_exclude;
  scratch_global_exclude |= (1u << rv) | (1u << rb);

  int cur_window = -1; /* base offset of the window currently in rb */
  int have_val = 0;
  uint32_t cur_val = 0;
  for (int k = 0; k < n; ++k)
  {
    int off = items[k].off;
    uint32_t val = items[k].val;
    int window = off & ~0xFFF;
    int disp = off & 0xFFF;
    int base_reg;

    if (window == 0)
    {
      base_reg = ARM_SP; /* sp+0 — store directly off sp, no base register needed */
    }
    else
    {
      if (window != cur_window)
      {
        thumb_opcode op = th_add_imm(rb, ARM_SP, (uint32_t)window, flags_safe(), ENFORCE_ENCODING_NONE);
        if (is_valid_opcode(op))
          ot(op);
        else
        {
          load_full_const(rb, PREG_NONE, (uint32_t)window, 0);
          ot_check(th_add_reg(rb, ARM_SP, rb, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
        }
      }
      base_reg = rb;
    }
    cur_window = window;

    if (!have_val || cur_val != val)
    {
      load_immediate(rv, val, NULL, false);
      have_val = 1;
      cur_val = val;
    }

    if (!store_word_to_base(rv, base_reg, disp, 0))
    {
      /* disp <= 4092 always encodes via str.w; keep a correct fallback regardless. */
      ScratchRegAlloc sc = get_scratch_reg_with_save((1u << rv) | (1u << base_reg));
      load_immediate(sc.reg, (uint32_t)off, NULL, false);
      ot_check(th_str_reg(rv, ARM_SP, sc.reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      restore_scratch_reg(&sc);
    }
  }

  scratch_global_exclude = saved_excl;
  tcc_free(items);
}

/* Handle return value after call (MOP path).
 * The 'dest_mop' describes where the return value must be written.
 * mach_writeback_dest() handles all destination kinds:
 *   MACH_OP_REG   — emit MOV dest.r0, ARM_R0 when needed
 *   MACH_OP_SPILL — emit STR R0 to the spill slot
 *   MACH_OP_PARAM_STACK — emit STR R0 to the param stack slot
 *   MACH_OP_NONE  — no-op (void return or drop_value)
 * 64-bit pairs (int64, double, complex float) are split into lo/hi halves
 * via mach_make_lo_half / mach_make_hi_half (R0 → lo, R1 → hi). */
static void handle_return_value_mop(const MachineOperand *dest_mop, int drop_value)
{
  if (drop_value)
    return;
  if (dest_mop->is_64bit)
  {
    /* 64-bit return value: R0 = low word, R1 = high word (AAPCS). */
    MachineOperand lo = mach_make_lo_half(dest_mop);
    lo.btype = IROP_BTYPE_INT32;
    MachineOperand hi = mach_make_hi_half(dest_mop);
    hi.btype = IROP_BTYPE_INT32;
    mach_writeback_dest(&lo, ARM_R0);
    mach_writeback_dest(&hi, ARM_R1);
    return;
  }
  mach_writeback_dest(dest_mop, ARM_R0);
}

/* ======================================================================== */

/* tcc_gen_machine_func_call_mop — MOP-path function call code generator.
 *
 * The function target and return-value destination are passed as MachineOperands.
 * The call_id_op is always an immediate IROperand (no fill needed).
 *
 * Phase 5g: func_mop replaces the old filled IROperand func_target.
 * gcall_or_jump_mop() replaces gcall_or_jump_ir().
 */
ST_FUNC void tcc_gen_machine_func_call_mop(MachineOperand func_mop, IROperand call_id_op, MachineOperand dest_mop,
                                           int drop_value, TCCIRState *ir, int call_idx)
{
  /* === Validation === */
  if (irop_is_none(call_id_op) || !ir)
    tcc_error("compiler_error: func_call_op requires call_id+ir");

  const int call_id = TCCIR_DECODE_CALL_ID(call_id_op.u.imm32);
  const int argc_hint = TCCIR_DECODE_CALL_ARGC(call_id_op.u.imm32);

  ThumbGenCallSite *call_site = thumb_get_call_site_for_id(call_id);
  if (!call_site)
    tcc_error("compiler_error: no call site found for call_id=%d", call_id);

  /* === Build ABI layout === */
  TCCAbiCallLayout layout;
  memset(&layout, 0, sizeof(layout));

  IROperand *args = NULL;
  MachineOperand *mops = NULL;
  const int argc = thumb_build_call_layout_from_ir(ir, call_idx, call_id, argc_hint, &layout, &args, &mops);
  if (argc < 0)
    tcc_error("compiler_error: failed to build call layout for call_id=%d", call_id);

  int stack_size = (argc > 0) ? (int)layout.stack_size : 0;

  /* === Setup call context === */
  CallGenContext ctx = {
      .call_site = call_site,
      .layout = &layout,
      .args = args,
      .mops = mops,
      .argc = argc,
      .stack_size = stack_size,
  };

  /* Set tail_call_pending if this is a tail-call-only function. */
  if (ir->tail_call_only)
    tail_call_pending = 1;

  /* === Preserve nested call registers (R0-R3, R9) via STR to frame ===
   * Instead of PUSH/POP (which moves SP), store to the pre-reserved
   * nested-call save area in the frame.  SP stays fixed. */
  int arg_regs_in_use = call_site->registers_map & 0x0F;
  int arg_regs_save_mask = tail_call_pending ? 0 : (arg_regs_in_use);

  /* On yasos with no-pic-data-is-text-relative, R9 holds the GOT base and is
   * caller-saved.  Save it alongside the nested-call argument registers so it
   * is restored after the callee returns. */
  if (!tail_call_pending && text_and_data_separation)
    arg_regs_save_mask |= (1 << ARM_R9);

  /* Save nested-call registers to pre-reserved frame area via STR.
   * The nested save area is at [SP + ir->call_outgoing_size]. */
  int nested_save_sp_offset = ir ? ir->call_outgoing_size : 0;
  int nested_save_count = 0;
  if (arg_regs_save_mask)
  {
    for (int r = 0; r < 16; r++)
    {
      if (arg_regs_save_mask & (1 << r))
      {
        store_word_to_stack(r, nested_save_sp_offset + nested_save_count * 4);
        nested_save_count++;
      }
    }
  }

  /* Stack args are already placed in the pre-reserved outgoing area at [SP+0].
   * No need to adjust SP — the area was allocated in the prologue. */
  stack_size = (stack_size + 7) & ~7; /* 8-byte align */

  /* === Save scratch exclusion state === */
  uint32_t saved_scratch_exclude = scratch_global_exclude;

  /* === Pre-save indirect call target if it resides in an argument register ===
   *
   * When a function pointer is allocated to R0-R3 by the register allocator,
   * the argument placement phase will overwrite those registers.  Pre-move the
   * pointer to a safe register before argument setup.
   *
   * Phase 5g: operates on MachineOperand func_mop instead of filled IROperand.
   */
  {
    const int is_direct = (func_mop.kind == MACH_OP_SYMBOL || func_mop.kind == MACH_OP_IMM);
    if (!is_direct && func_mop.kind == MACH_OP_REG && !func_mop.needs_deref && func_mop.u.reg.r0 >= 0 &&
        func_mop.u.reg.r0 <= 3)
    {
      /* Find a free register outside R0-R3, R12 (stack-arg scratch), SP, PC. */
      uint32_t exclude = scratch_global_exclude | 0x0Fu | (1u << R_IP) | (1u << R_SP) | (1u << R_PC);
      int safe_reg = PREG_NONE;
      if (ir)
        safe_reg = tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, exclude, ir->leaffunc);

      if (safe_reg == PREG_NONE || safe_reg < 0 || safe_reg >= 16 || safe_reg == R_SP || safe_reg == R_PC)
        tcc_error("compiler_error: func_call_mop: cannot find safe register "
                  "to pre-save indirect call target (R%d)",
                  func_mop.u.reg.r0);

      /* Move function pointer from arg reg to safe reg. */
      thumb_shift no_shift = {THUMB_SHIFT_NONE, 0, THUMB_SHIFT_IMMEDIATE};
      ot_check_mov_reg(safe_reg, func_mop.u.reg.r0, flags_safe(), no_shift, ENFORCE_ENCODING_NONE,
                       false);

      /* Rewrite func_mop to point to the safe register. */
      func_mop.kind = MACH_OP_REG;
      func_mop.u.reg.r0 = safe_reg;
      func_mop.u.reg.r1 = -1;
      func_mop.needs_deref = false;

      /* Protect the safe register from scratch allocation during arg setup. */
      scratch_global_exclude |= (1u << safe_reg);
    }
  }

  /* === Build register argument moves === */
  ThumbArgMove reg_moves[8];
  int reg_move_count = build_register_arg_moves(&ctx, reg_moves);

  /* === Compute arg_move_dst_mask and identity-move protection ===
   *
   * Stack arguments are placed BEFORE register argument moves so that
   * R0-R3 (non-identity move destinations) can serve as scratch registers
   * for stack arg stores, saving 2 bytes per store (16-bit vs 32-bit encoding).
   *
   * arg_move_dst_mask: registers written by explicit (non-identity) reg moves.
   *   These will be overwritten by the moves, so they're safe as scratch.
   * identity_mask: registers where the reg allocator already placed the correct
   *   value (no move entry created).  These MUST be protected from clobbering. */
  {
    uint32_t arg_move_dst_mask = 0;
    for (int i = 0; i < reg_move_count; i++)
      arg_move_dst_mask |= arg_move_write_set(&reg_moves[i]);

    /* Compute all register-arg destination registers from the ABI layout. */
    uint32_t all_reg_arg_dst = 0;
    for (int i = 0; i < ctx.argc; i++)
    {
      const TCCAbiArgLoc *loc = &ctx.layout->locs[i];
      if (loc->kind == TCC_ABI_LOC_REG || loc->kind == TCC_ABI_LOC_REG_STACK)
      {
        int base = ARM_R0 + loc->reg_base;
        for (int w = 0; w < loc->reg_count; w++)
          all_reg_arg_dst |= (1u << (base + w));
      }
    }

    /* Protect identity-move registers (value already in place, no move entry). */
    uint32_t identity_mask = all_reg_arg_dst & ~arg_move_dst_mask;
    scratch_global_exclude |= identity_mask;

    ctx.arg_move_dst_mask = arg_move_dst_mask;
  }

  /* Pre-save stack args sourcing from R0-R3 before register shuffle */
  presave_stack_args_from_arg_regs(&ctx);

  /* === Place stack arguments FIRST ===
   * R0-R3 that are non-identity move destinations can be used as scratch
   * via arg_move_dst_mask in find_call_scratch, yielding 16-bit STR
   * encodings instead of 32-bit STR.W with R12. */
  place_stack_arguments(&ctx);

  /* === Now block all R0-R3 and emit register argument moves === */
  scratch_global_exclude |= 0x0F;
  thumb_emit_parallel_arg_moves(reg_moves, reg_move_count);

  /* === Tail call: tear down frame before branching === */
  if (tail_call_pending)
  {
    /* For indirect calls, the target may be in a callee-saved register that
     * will be popped.  Move it to R_IP (R12) before frame teardown. */
    if (func_mop.kind == MACH_OP_REG && !func_mop.needs_deref &&
        func_mop.u.reg.r0 >= R4 && func_mop.u.reg.r0 <= R11)
    {
      ot_check_mov_reg(R_IP, func_mop.u.reg.r0, flags_safe(),
                       THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
      func_mop.u.reg.r0 = R_IP;
    }
    if (epilogue_stack_dealloc > 0)
      gadd_sp_ex(epilogue_stack_dealloc, R_IP);
    /* Only pop true callee-saved registers (R4-R11).  R0-R3 may be pushed
     * for alignment but now hold call arguments — popping them would clobber
     * the prepared args.  Skip non-callee slots FIRST (they sit at lower
     * addresses after push), then pop callee-saved from correct position. */
    uint32_t callee_pop = pushed_registers & 0x0FF0u; /* R4-R11 only */
    uint32_t non_callee = pushed_registers & ~callee_pop & ~(1u << R_LR) & ~(1u << R_PC);
    int non_callee_bytes = __builtin_popcount(non_callee) * 4;
    if (non_callee_bytes > 0)
      gadd_sp_ex(non_callee_bytes, R_IP);
    if (callee_pop)
      ot_check(th_pop(callee_pop));
  }

  /* === Emit call === */
  gcall_or_jump_mop(0, func_mop);
  /* Restore scratch register exclusion */
  scratch_global_exclude = saved_scratch_exclude;

  if (tail_call_pending)
  {
    tail_call_pending = 0;
    goto call_cleanup;
  }

  handle_return_value_mop(&dest_mop, drop_value);

  /* === Cleanup: restore nested-call saved registers via LDR === */
  if (arg_regs_save_mask)
  {
    int restore_idx = 0;
    for (int r = 0; r < 16; r++)
    {
      if (arg_regs_save_mask & (1 << r))
      {
        int off = nested_save_sp_offset + restore_idx * 4;
        /* R9 restore in text_and_data_separation mode needs the write guard
         * temporarily lifted — the safety check blocks all R9 writes, but
         * we are legitimately restoring it after a call. */
        if (r == ARM_R9 && text_and_data_separation)
          allow_r9_write = 1;
        if (!load_word_from_base(r, ARM_SP, off, 0))
        {
          ScratchRegAlloc osc = get_scratch_reg_with_save((1u << r));
          load_immediate(osc.reg, off, NULL, false);
          ot_check(th_ldr_reg(r, ARM_SP, osc.reg, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
          restore_scratch_reg(&osc);
        }
        if (r == ARM_R9 && text_and_data_separation)
          allow_r9_write = 0;
        restore_idx++;
      }
    }
  }

  call_site->registers_map &= ~0x0F; /* Clear R0-R3 */

call_cleanup:
  if (args)
    tcc_free(args);
  if (mops)
    tcc_free(mops);
  if (layout.locs)
    tcc_free(layout.locs);
}

/* Check if a backward branch to target_ir can use a narrow encoding.
 * For backward branches, the target code address is already known in
 * ir_to_code_mapping (it was emitted earlier in this pass).
 * current_ir_idx is the IR index of the branch instruction itself.
 * Returns 1 if narrow encoding fits, 0 otherwise. */
static int can_narrow_backward_branch(int32_t target_ir, int is_conditional, int current_ir_idx)
{
  TCCIRState *ir = tcc_state->ir;
  if (!ir || !ir->ir_to_code_mapping)
    return 0;
  if (target_ir < 0 || target_ir >= ir->ir_to_code_mapping_size)
    return 0;

  /* Forward branches have uninitialized ir_to_code_mapping[target_ir] (still 0).
   * Only narrow genuinely backward branches where target was already emitted. */
  if (target_ir >= current_ir_idx)
    return 0;

  int target_addr = (int)ir->ir_to_code_mapping[target_ir];
  /* ind is the current code address where the branch will be emitted.
   * offset = target - (source + 4) for Thumb pipeline. */
  int offset = target_addr - ind - 4;

  /* Only backward branches (negative offset) are safe to narrow here */
  if (offset >= 0)
    return 0;

  return is_conditional ? branch_fits_t1(offset) : branch_fits_t2(offset);
}

ST_FUNC int tcc_gen_machine_jump_mop(TccIrOp op, int32_t target_ir, int ir_idx)
{

  if (dry_run_state.active)
  {
    /* Emit 32-bit placeholder for code size tracking */
    ot_check(th_b_t4(0));
    return 4;
  }

  /* Real pass: try narrow encoding for backward branches */
  if (can_narrow_backward_branch(target_ir, 0, ir_idx))
  {
    ot_check(th_b_t2(0)); /* 16-bit unconditional */
    return 2;
  }
  else
  {
    ot_check(th_b_t4(0)); /* 32-bit unconditional */
    return 4;
  }
}

ST_FUNC int tcc_gen_machine_conditional_jump_mop(int32_t condition, TccIrOp op, int32_t target_ir, int ir_idx)
{
  int cond = mapcc(condition);

  if (dry_run_state.active)
  {
    /* Emit 32-bit placeholder for code size tracking */
    ot_check(th_b_t3(cond, 0));
    return 4;
  }

  /* Real pass: try narrow encoding for backward branches */
  if (can_narrow_backward_branch(target_ir, 1, ir_idx))
  {
    ot_check(th_b_t1(cond, 0)); /* 16-bit conditional */
    return 2;
  }
  else
  {
    ot_check(th_b_t3(cond, 0)); /* 32-bit conditional */
    return 4;
  }
}

/* Return the maximum bytes a pending literal pool dump could insert.
 * Used for CBZ/CBNZ distance safety checks. */
ST_FUNC int tcc_gen_machine_pending_pool_size(void)
{
  int count = dry_run_state.active ? dry_run_literal_pool_count : thumb_gen_state.literal_pool_count;
  return count * 4 + (count > 0 ? 2 : 0); /* entries + possible alignment padding */
}

/* Emit CBZ/CBNZ: combined compare-zero + branch in a single 16-bit instruction.
 * rn must be r0-r7, target must be forward within 126 bytes.
 * Returns the instruction size (always 2). */
ST_FUNC int tcc_gen_machine_cbz_jump_mop(int rn, int nonzero, int32_t target_ir, int ir_idx)
{
  ot_check(th_cbz((uint16_t)rn, 0, (uint32_t)nonzero));
  return 2;
}

/* Set static chain register: MOV R10, R7 (FP) */
ST_FUNC void tcc_gen_machine_set_chain(void)
{
  int chain_reg = architecture_config.static_chain_reg;
  thumb_shift no_shift = {THUMB_SHIFT_NONE, 0, THUMB_SHIFT_IMMEDIATE};
  /* MOV chain_reg, R_FP (R7 on ARM Thumb) */
  ot_check_mov_reg(chain_reg, R_FP, flags_safe(), no_shift, ENFORCE_ENCODING_NONE, false);
}

/* Reload static chain register from the chain save slot at [FP - 4].
 * Called after function calls in nested functions with has_static_chain,
 * because trampoline calls can clobber R10. */
ST_FUNC void tcc_gen_machine_restore_chain(void)
{
  int chain_reg = architecture_config.static_chain_reg;
  /* LDR chain_reg, [FP, #-4] */
  if (!load_word_from_base(chain_reg, R_FP, 4, 1))
  {
    /* Fallback for large offset (should not happen for -4) */
    ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(4, 1, (1u << chain_reg) | (1u << R_FP));
    int rr = rr_alloc.reg;
    ot_check(th_ldr_reg(chain_reg, R_FP, rr, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    restore_scratch_reg(&rr_alloc);
  }
}

/* Store parent FP (R7) into chain slot in .data for nested function trampoline.
 * src1 carries the chain slot symbol via SYMREF so we can emit a relocation. */
ST_FUNC void tcc_gen_machine_init_chain_slot(IROperand src1)
{
  /* Extract the chain slot Sym* from the IROperand */
  Sym *chain_sym = irop_get_sym(src1);
  if (!chain_sym)
    tcc_error("internal error: INIT_CHAIN_SLOT without chain slot symbol");

  /* Get a scratch register to hold the chain slot address */
  ScratchRegAlloc scratch = get_scratch_reg_with_save(0);

  /* Load chain slot address into scratch register via literal pool. */
  _lfc_sym = chain_sym;
  load_full_const(scratch.reg, PREG_NONE, 0, 0);

  /* STR R7, [scratch, #0] — store frame pointer into chain slot */
  ot_check_str_imm(R_FP, scratch.reg, 0, 6, ENFORCE_ENCODING_NONE);

  /* Restore scratch register */
  restore_scratch_reg(&scratch);
}

/* Called at end of each IR instruction to clean up scratch register state.
 * - Restores any pushed scratch registers (POP in reverse push order)
 * - Resets global exclusion mask for next instruction */
ST_FUNC void tcc_gen_machine_end_instruction(void)
{
  restore_all_pushed_scratch_regs();
}

/* tcc_gen_machine_vla_mop: MachineOperand-based entry point for VLA operations.
 *
 *   VLA_ALLOC:      src1=size(bytes), src2=alignment(IMM bytes), dest unused
 *   VLA_SP_SAVE:    dest=save slot, src1/src2 unused
 *   VLA_SP_RESTORE: src1=save slot, dest/src2 unused
 *
 * Gate: !ir->has_static_chain (VLA ops are always 32-bit pointer/int sized).
 */
ST_FUNC void tcc_gen_machine_vla_mop(MachineOperand dest, MachineOperand src1, MachineOperand src2, TccIrOp op)
{
  MachineCodegenContext ctx = {0};
  switch (op)
  {
  case TCCIR_OP_VLA_ALLOC:
  {
    /* src1=size (may be register or spilled); src2=alignment (IMM or NONE). */
    int align = (src2.kind == MACH_OP_IMM) ? (int)src2.u.imm.val : 8;
    if (align < 8)
      align = 8;
    if (align & (align - 1))
      tcc_error("alignment is not a power of 2: %i", align);

    /* Load size into a working register — it's dead after this op. */
    int r = mach_ensure_in_reg(&ctx, &src1, 0);
    if (r == R_SP)
      tcc_error("compiler_error: VLA alloc picked SP as temp");

    /* r = SP - r  (subtract size from stack pointer) */
    ot_check(th_sub_reg(r, R_SP, r, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));

    if (align > 1)
    {
      /* Align down: r &= ~(align-1).  Try immediate BIC first. */
      if (!ot(th_bic_imm(r, r, (uint32_t)(align - 1), flags_safe(), ENFORCE_ENCODING_NONE)))
      {
        /* Fallback: materialize mask in a scratch register. */
        int mask_reg = mach_alloc_scratch(&ctx, 1u << (uint32_t)r);
        if (!ot(th_generic_mov_imm(mask_reg, align - 1)))
          load_full_const(mask_reg, PREG_NONE, LFC_SPLIT(align - 1));
        ot_check(th_bic_reg(r, r, mask_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      }
    }

    ot_check_mov_reg(R_SP, r, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
    break;
  }
  case TCCIR_OP_VLA_SP_SAVE:
  {
    /* Fast path: when dest is a register-allocated vreg, copy SP directly into
     * its register — saves the scratch-mov + writeback-mov pair that the
     * generic path would emit.  Triggered by the alloca-load-fwd IR pass
     * which rewrites a `VLA_SP_SAVE slot; LOAD vreg <- slot` pair into a
     * single `VLA_SP_SAVE vreg`. */
    if (dest.kind == MACH_OP_REG && !dest.needs_deref &&
        dest.u.reg.r0 != (int)PREG_REG_NONE)
    {
      ot_check_mov_reg((uint32_t)dest.u.reg.r0, R_SP, flags_safe(),
                       THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
      break;
    }
    /* Save current SP to the destination save slot via a scratch register. */
    ScratchRegAlloc sp_scratch = get_scratch_reg_with_save(0);
    ot_check_mov_reg(sp_scratch.reg, R_SP, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                     false);
    mach_writeback_dest(&dest, sp_scratch.reg);
    restore_scratch_reg(&sp_scratch);
    break;
  }
  case TCCIR_OP_VLA_SP_RESTORE:
  {
    /* Load the saved SP from src1 into a register, then restore SP. */
    int saved_sp = mach_ensure_in_reg(&ctx, &src1, 0);
    ot_check_mov_reg(R_SP, saved_sp, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
    break;
  }
  default:
    tcc_error("compiler_error: tcc_gen_machine_vla_mop unsupported op %d", op);
  }
  mach_release_all(&ctx);
}

/* Block copy from const data section to stack using LDM/STM.
 * dest = STACKOFF (destination stack offset, is_local=1)
 * src  = SYMREF (anonymous symbol in rodata)
 * size = number of bytes to copy (must be multiple of 4)
 *
 * Generated code for 20 bytes (5 words):
 *   LDR   r_src, [PC, #lit_pool]    ; load rodata address
 *   ADD   r_dst, FP/SP, #stack_off  ; compute stack dest
 *   LDMIA r_src!, {r0, r1, r2, r3}  ; load 4 words from rodata
 *   STMIA r_dst!, {r0, r1, r2, r3}  ; store 4 words to stack
 *   LDR   r0, [r_src]               ; load remaining word
 *   STR   r0, [r_dst]               ; store remaining word
 */
/* tcc_gen_machine_select_mop: Conditional select using ITE block.
 * Emits: ITE <cond>; MOV dest, then_val; MOV dest, else_val
 *
 * For simple register/immediate operands, this is 3 instructions (ITE + 2 MOVs)
 * instead of 5+ (B.cond + MOV + B + MOV + ...) with branching.
 */
/* Check if a MachineOperand can be materialized in exactly one instruction.
 * Returns 1 for: IMM (any value), REG (no deref), SYMBOL (no deref), SPILL (no deref).
 * Returns 0 for: multi-instruction sequences (deref, chain_rel, etc). */
static int select_can_inline(const MachineOperand *op)
{
  switch (op->kind)
  {
  case MACH_OP_IMM:
    return 1; /* MOV/MOVW/MVN or literal pool LDR — always 1 instruction */
  case MACH_OP_REG:
    return !op->needs_deref; /* MOV reg is 1 instr; deref needs LDR too */
  case MACH_OP_SYMBOL:
    /* A symbol address is a single literal-pool LDR only in the plain,
     * non-PIC, non-separated layout.  Under PIC/PIE or text+data separation it
     * expands to a multi-instruction GOT/GOTOFF sequence (ldr GOT-slot; add r9;
     * ldr; ...).  Emitting that "inline" inside an IT block predicates only the
     * FIRST instruction and lets the remaining ones run unconditionally, which
     * clobbers the select result with the else-operand's address.  Force
     * pre-materialization into a scratch register in those modes. */
    return !op->needs_deref && !pic && !text_and_data_separation;
  case MACH_OP_SPILL:
    return !op->needs_deref; /* LDR from stack is 1 instr; deref (VT_LLOCAL) needs 2 */
  case MACH_OP_FRAME_ADDR:
    return 1; /* ADD reg, FP, #off is 1 instr */
  default:
    return 0;
  }
}

/* Emit a single-instruction materialization of 'op' into 'reg'.
 * Caller must ensure select_can_inline(op) returned 1. */
static void select_emit_inline(MachineCodegenContext *ctx, const MachineOperand *op, int reg)
{
  switch (op->kind)
  {
  case MACH_OP_IMM:
  {
    thumb_opcode imm_op = th_generic_mov_imm((uint32_t)reg, (int)op->u.imm.val);
    if (imm_op.size != 0)
      ot(imm_op);
    else
      load_full_const(reg, PREG_NONE, LFC_SPLIT(op->u.imm.val));
    break;
  }
  case MACH_OP_REG:
    ot_check_mov_reg(reg, op->u.reg.r0, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                     true);
    break;
  case MACH_OP_SYMBOL:
  {
    Sym *raw_sym = op->u.sym.sym;
    Sym *sym = raw_sym ? validate_sym_for_reloc(raw_sym) : NULL;
    tcc_machine_load_constant(reg, PREG_REG_NONE, op->u.sym.addend, 0, sym);
    break;
  }
  case MACH_OP_SPILL:
    tcc_machine_load_spill_slot(reg, op->u.spill.offset);
    break;
  case MACH_OP_FRAME_ADDR:
    tcc_machine_addr_of_stack_slot(reg, op->u.frame.offset, 0);
    break;
  default:
    tcc_error("compiler_error: select_emit_inline: unhandled kind %d", (int)op->kind);
    break;
  }
}

ST_FUNC void tcc_gen_machine_select_mop(MachineOperand then_val, MachineOperand else_val, MachineOperand dest,
                                        int cond_code)
{
  MachineCodegenContext mctx = {0};

  int cond = mapcc(cond_code);

  /* Get destination register */
  int dest_reg = mach_get_dest_reg(&mctx, &dest, 0);
  uint32_t excl = (1u << (uint32_t)dest_reg);

  /* Determine if each operand can be materialized in exactly one instruction.
   * If so, we can emit it directly inside the ITE block into dest_reg,
   * saving scratch registers and pre-materialization instructions.
   *
   * Emitting inside the IT block is preferred because:
   * - It avoids flag clobber (MOVS before ITE would destroy CMP flags)
   * - It saves scratch registers (no pre-materialization needed)
   * - It produces smaller code */
  int then_inline = select_can_inline(&then_val);
  int else_inline = select_can_inline(&else_val);

  int then_reg = -1, else_reg = -1;

  /* Pre-materialize operands that need multi-instruction sequences.
   * These are loaded into scratch registers BEFORE the ITE block. */
  if (!then_inline)
  {
    then_reg = mach_ensure_in_reg(&mctx, &then_val, excl);
    excl |= (1u << (uint32_t)then_reg);
  }
  if (!else_inline)
  {
    else_reg = mach_ensure_in_reg(&mctx, &else_val, excl);
    excl |= (1u << (uint32_t)else_reg);
  }

  /* Identity-then shortcut: if the then-value is already in dest_reg, the
   * predicated mov would be `movXX dest, dest` — a real instruction inside an
   * IT block (the usual elision in ot_check_mov_reg is suppressed by in_it).
   * Emit `IT <inv_cond>` + the else mov instead.  Saves one instruction. */
  int then_is_identity = 0;
  if (then_inline && then_val.kind == MACH_OP_REG && !then_val.needs_deref &&
      (int)then_val.u.reg.r0 == dest_reg)
    then_is_identity = 1;
  else if (!then_inline && then_reg == dest_reg)
    then_is_identity = 1;

  if (then_is_identity)
  {
    int inv_cond = cond ^ 1;
    th_literal_pool_reserve_upcoming_bytes(8); /* IT(2) + instr(2-4) */
    ot_check(th_it((uint16_t)inv_cond, 0x8u)); /* IT <inv_cond>, single insn */
    if (else_inline)
      select_emit_inline(&mctx, &else_val, dest_reg);
    else
      ot_check_mov_reg(dest_reg, else_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, true);
    mach_writeback_dest(&dest, dest_reg);
    mach_release_all(&mctx);
    return;
  }

  /* ITE mask: the second instruction uses the opposite condition.
   * mask encoding: bit3 = E_flag for 2nd instr, bit2 = end marker.
   * E_flag = opposite of cond[0], so: mask = ((cond[0]^1) << 3) | (1 << 2) */
  uint32_t ite_mask = (uint32_t)(((cond & 1) ^ 1) << 3) | 0x4u;

  /* Reserve literal pool space to prevent pool dumps inside the IT block */
  th_literal_pool_reserve_upcoming_bytes(10); /* ITE(2) + instr(2-4) + instr(2-4) */

  ot_check(th_it((uint16_t)cond, (uint16_t)ite_mask));

  /* Emit the Then instruction inside IT block */
  if (then_inline)
    select_emit_inline(&mctx, &then_val, dest_reg);
  else
    ot_check_mov_reg(dest_reg, then_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                     true);

  /* Emit the Else instruction inside IT block */
  if (else_inline)
    select_emit_inline(&mctx, &else_val, dest_reg);
  else
    ot_check_mov_reg(dest_reg, else_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                     true);

  mach_writeback_dest(&dest, dest_reg);
  mach_release_all(&mctx);
}

ST_FUNC void tcc_gen_machine_block_copy_mop(TCCIRState *ir, IROperand dest, IROperand src, int size)
{
  if (size <= 0 || (size & 3))
    tcc_error("compiler_error: block_copy size must be positive multiple of 4, got %d", size);

  /* Get the source symbol from the SYMREF operand */
  IRPoolSymref *symref = irop_get_symref_ex(ir, src);
  if (!symref || !symref->sym)
    tcc_error("compiler_error: block_copy source is not a valid symbol reference");
  Sym *sym = validate_sym_for_reloc(symref->sym);

  /* Get the destination stack offset */
  int frame_offset = (int)irop_get_imm64_ex(ir, dest);

  /* For large copies, call memcpy instead of inline LDM/STM.
   * Compute dest address into r0 BEFORE pushing lr, since the address is
   * sp-relative and pushing changes sp.  The BL to memcpy clobbers lr,
   * so we must save/restore it for leaf functions whose prologue didn't. */
  if (size >= 64)
  {
    tcc_machine_addr_of_stack_slot(R0, frame_offset, 0 /* not param */);
    tcc_machine_load_constant(R1, PREG_REG_NONE, symref->addend, 0, sym);
    tcc_machine_load_constant(R2, PREG_REG_NONE, size, 0, NULL);
    int need_lr_save = ir->leaffunc;
    if (need_lr_save)
      ot_check(th_push(1u << ARM_LR));
    Sym *memcpy_sym = external_global_sym(tok_alloc_const("memcpy"), &func_old_type);
    MachineOperand func_mop = {0};
    func_mop.kind = MACH_OP_SYMBOL;
    func_mop.u.sym.sym = memcpy_sym;
    func_mop.u.sym.addend = 0;
    if (text_and_data_separation)
      ot_check(th_push((uint16_t)((1 << R9) | (1 << R12))));
    gcall_or_jump_mop(0, func_mop);
    if (text_and_data_separation)
      ot_check(th_pop((uint16_t)((1 << R9) | (1 << R12))));
    if (need_lr_save)
      ot_check(th_pop(1u << ARM_LR));
    return;
  }

  int nwords = size / 4;

  /* Allocate pointer registers first and compute addresses BEFORE allocating
   * data registers.  Data register saves may use PUSH which modifies SP,
   * so all SP-relative address computation must happen before that. */
  ScratchRegAlloc src_scratch = get_scratch_reg_with_save(0);
  int r_src = src_scratch.reg;
  ScratchRegAlloc dst_scratch = get_scratch_reg_with_save(1u << (uint32_t)r_src);
  int r_dst = dst_scratch.reg;

  /* Load source address (rodata symbol) into r_src */
  tcc_machine_load_constant(r_src, PREG_REG_NONE, symref->addend, 0, sym);

  /* Compute destination stack address into r_dst BEFORE any data reg saves
   * that might change SP via PUSH */
  tcc_machine_addr_of_stack_slot(r_dst, frame_offset, 0 /* not param */);

  /* Now allocate data registers for LDM/STM.  Even if these saves use PUSH
   * and modify SP, we've already captured the destination address in r_dst. */
  int max_data = nwords < 4 ? nwords : 4;
  if (max_data < 1)
    max_data = 1;

  ScratchRegAlloc data_scratches[4];
  int data_regs[4];
  int ndata = 0;
  uint32_t exclude = (1u << (uint32_t)r_src) | (1u << (uint32_t)r_dst);
  for (int k = 0; k < max_data; k++)
  {
    data_scratches[k] = get_scratch_reg_with_save(exclude);
    data_regs[k] = data_scratches[k].reg;
    exclude |= (1u << (uint32_t)data_regs[k]);
    ndata++;
  }

  int remaining_words = nwords;

  /* Process in chunks of ndata words using LDM/STM with writeback */
  while (remaining_words >= ndata && ndata >= 2)
  {
    uint32_t regset = 0;
    for (int j = 0; j < ndata; j++)
      regset |= (1u << (uint32_t)data_regs[j]);

    ot_check(th_ldm(r_src, regset, 1 /* writeback */, ENFORCE_ENCODING_NONE));
    ot_check(th_stm(r_dst, regset, 1 /* writeback */, ENFORCE_ENCODING_NONE));
    remaining_words -= ndata;
  }

  /* Handle remaining words individually */
  int dr = data_regs[0]; /* first data register */
  while (remaining_words > 0)
  {
    ot_check_ldr_imm(dr, r_src, 0, 6, ENFORCE_ENCODING_NONE);
    ot_check_str_imm(dr, r_dst, 0, 6, ENFORCE_ENCODING_NONE);
    if (remaining_words > 1)
    {
      if (!ot(th_add_imm(r_src, r_src, 4, flags_safe(), ENFORCE_ENCODING_NONE)))
        tcc_error("compiler_error: block_copy cannot advance source pointer");
      if (!ot(th_add_imm(r_dst, r_dst, 4, flags_safe(), ENFORCE_ENCODING_NONE)))
        tcc_error("compiler_error: block_copy cannot advance dest pointer");
    }
    remaining_words--;
  }

  /* Restore all scratch registers in reverse order: data regs first, then ptrs */
  for (int k = ndata - 1; k >= 0; k--)
    restore_scratch_reg(&data_scratches[k]);
  restore_scratch_reg(&dst_scratch);
  restore_scratch_reg(&src_scratch);
}

ST_FUNC void tcc_gen_machine_spill_block_copy(int32_t src_spill_off, int32_t dst_spill_off, int nwords)
{
  ScratchRegAlloc src_scratch = get_scratch_reg_with_save(0);
  int r_src = src_scratch.reg;
  ScratchRegAlloc dst_scratch = get_scratch_reg_with_save(1u << (uint32_t)r_src);
  int r_dst = dst_scratch.reg;

  tcc_machine_addr_of_stack_slot(r_src, src_spill_off, 0);
  tcc_machine_addr_of_stack_slot(r_dst, dst_spill_off, 0);

  int max_data = nwords < 4 ? nwords : 4;
  if (max_data < 1)
    max_data = 1;

  ScratchRegAlloc data_scratches[4];
  int data_regs[4];
  int ndata = 0;
  uint32_t exclude = (1u << (uint32_t)r_src) | (1u << (uint32_t)r_dst);
  for (int k = 0; k < max_data; k++)
  {
    data_scratches[k] = get_scratch_reg_with_save(exclude);
    data_regs[k] = data_scratches[k].reg;
    exclude |= (1u << (uint32_t)data_regs[k]);
    ndata++;
  }

  int remaining = nwords;

  while (remaining >= ndata && ndata >= 2)
  {
    uint32_t regset = 0;
    for (int j = 0; j < ndata; j++)
      regset |= (1u << (uint32_t)data_regs[j]);
    ot_check(th_ldm(r_src, regset, 1 /* writeback */, ENFORCE_ENCODING_NONE));
    ot_check(th_stm(r_dst, regset, 1 /* writeback */, ENFORCE_ENCODING_NONE));
    remaining -= ndata;
  }

  int dr = data_regs[0];
  while (remaining > 0)
  {
    ot_check_ldr_imm(dr, r_src, 0, 6, ENFORCE_ENCODING_NONE);
    ot_check_str_imm(dr, r_dst, 0, 6, ENFORCE_ENCODING_NONE);
    if (remaining > 1)
    {
      if (!ot(th_add_imm(r_src, r_src, 4, flags_safe(), ENFORCE_ENCODING_NONE)))
        tcc_error("compiler_error: spill_block_copy cannot advance source pointer");
      if (!ot(th_add_imm(r_dst, r_dst, 4, flags_safe(), ENFORCE_ENCODING_NONE)))
        tcc_error("compiler_error: spill_block_copy cannot advance dest pointer");
    }
    remaining--;
  }

  for (int k = ndata - 1; k >= 0; k--)
    restore_scratch_reg(&data_scratches[k]);
  restore_scratch_reg(&dst_scratch);
  restore_scratch_reg(&src_scratch);
}

ST_FUNC void tcc_gen_machine_trap_mop(void)
{
  /* Emit UDF #0xfe - Undefined instruction for trap */
  ot_check(th_udf(0xfe, ENFORCE_ENCODING_NONE));
}

ST_FUNC void tcc_gen_machine_prefetch_mop(MachineOperand addr, int rw)
{
  /* Emit PLD (Preload Data) or PLDW (Preload Data with intent to Write)
   * based on the rw hint.
   *
   * PLD/PLDW are hints to the memory system that data may be needed soon.
   * They don't wait for the data and don't fault if the address is invalid.
   *
   * We support several addressing modes:
   * - Register indirect: [Rn] -> use th_pld_imm with offset 0
   * - Register + immediate offset: [Rn, #imm]
   * - Literal (PC-relative): label
   */
  (void)rw; /* PLD/PLDW distinction may not be supported on all ARM variants */

  switch (addr.kind)
  {
  case MACH_OP_REG:
  {
    /* Register indirect: PLD [Rn] */
    int reg = addr.u.reg.r0;
    ot_check(th_pld_imm((uint32_t)reg, 0, 0));
    break;
  }
  case MACH_OP_SPILL:
  {
    /* Spill slot: compute address (FP + offset) then PLD */
    int32_t offset = addr.u.spill.offset;
    if (offset != 0)
    {
      ScratchRegAlloc scr = get_scratch_reg_with_save(0);
      load_full_const(scr.reg, PREG_NONE, LFC_SPLIT(offset));
      ot_check(th_add_reg(scr.reg, R_FP, scr.reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      ot_check(th_pld_imm(scr.reg, 0, 0));
      restore_scratch_reg(&scr);
    }
    else
    {
      ot_check(th_pld_imm(R_FP, 0, 0));
    }
    break;
  }
  case MACH_OP_IMM:
  {
    /* For immediate addresses, load into a register first */
    ScratchRegAlloc scr = get_scratch_reg_with_save(0);
    load_full_const(scr.reg, PREG_NONE, LFC_SPLIT(addr.u.imm.val));
    ot_check(th_pld_imm(scr.reg, 0, 0));
    restore_scratch_reg(&scr);
    break;
  }
  case MACH_OP_SYMBOL:
  {
    /* For symbol addresses, load into a register first */
    ScratchRegAlloc scr = get_scratch_reg_with_save(0);
    _lfc_sym = addr.u.sym.sym;
    load_full_const(scr.reg, PREG_NONE, LFC_SPLIT(addr.u.sym.addend));
    ot_check(th_pld_imm(scr.reg, 0, 0));
    restore_scratch_reg(&scr);
    break;
  }
  case MACH_OP_FRAME_ADDR:
  {
    /* Frame address: FP + offset */
    int32_t offset = addr.u.frame.offset;
    if (offset != 0)
    {
      ScratchRegAlloc scr = get_scratch_reg_with_save(0);
      load_full_const(scr.reg, PREG_NONE, LFC_SPLIT(offset));
      ot_check(th_add_reg(scr.reg, R_FP, scr.reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      ot_check(th_pld_imm(scr.reg, 0, 0));
      restore_scratch_reg(&scr);
    }
    else
    {
      ot_check(th_pld_imm(R_FP, 0, 0));
    }
    break;
  }
  default:
    tcc_error("unsupported operand type for __builtin_prefetch");
  }
}

/* __builtin_setjmp implementation for ARM Thumb-2.
 *
 * Jump buffer layout (3 words, fits in the standard 5-word buffer):
 *   buf[0]  = frame pointer (R7/FP)
 *   buf[1]  = resume address (Thumb-bit set)
 *   buf[2]  = stack pointer (SP)
 *
 * Returns 0 on initial call, 1 when returning via longjmp.
 */
ST_FUNC void tcc_gen_machine_setjmp_mop(MachineOperand buf, MachineOperand dest)
{
  MachineCodegenContext ctx = {0};
  int buf_reg;

  if (buf.kind == MACH_OP_NONE)
  {
    buf_reg = mach_alloc_scratch(&ctx, 0);
    ot_check(th_mov_imm(buf_reg, 0, flags_safe(), ENFORCE_ENCODING_NONE));
  }
  else
  {
    buf_reg = mach_ensure_in_reg(&ctx, &buf, 0);
  }

  /* ---- save frame pointer ---- */
  ot_check_str_imm(R_FP, buf_reg, 0, 6, ENFORCE_ENCODING_NONE); /* r7  -> buf[0]  */

  /* ---- save SP ---- */
  ot_check_mov_reg(R_IP, R_SP, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
  ot_check_str_imm(R_IP, buf_reg, 8, 6, ENFORCE_ENCODING_NONE); /* SP -> buf[2] */

  /* ---- save resume address (ADR IP, resume_label) ---- */
  int adr_addr = ind;
  int adr_pc = adr_addr + 4;
  int adr_base = adr_pc & ~3;
  int resume_label_addr = adr_addr + 20; /* 4(ORR)+4(STR)+4(MOV)+4(B) after ADR */
  int adr_imm = resume_label_addr - adr_base;
  ot_check(th_adr_imm(R_IP, adr_imm, ENFORCE_ENCODING_32BIT));

  ot_check(th_orr_imm(R_IP, R_IP, 1, flags_safe(), ENFORCE_ENCODING_NONE)); /* Thumb bit */
  ot_check_str_imm(R_IP, buf_reg, 4, 6, ENFORCE_ENCODING_NONE);                              /* -> buf[1] */

  /* ---- normal path: return 0 ---- */
  int dest_reg = mach_get_dest_reg(&ctx, &dest, 0);
  ot_check(th_mov_imm(dest_reg, 0, flags_safe(), ENFORCE_ENCODING_32BIT)); /* dest = 0 */
  ot_check(th_b_t4(4));                                                                     /* B.W +4 (skip resume) */

  /* ---- resume_label: longjmp lands here ---- */
  ot_check(th_mov_imm(dest_reg, 1, flags_safe(), ENFORCE_ENCODING_32BIT)); /* dest = 1 */
  /* ---- end_label ---- */

  mach_writeback_dest(&dest, dest_reg);
  mach_release_all(&ctx);
}

/* Non-local goto setjmp: saves ALL callee-saved registers (r4-r11), SP,
 * and resume address in a 40-byte buffer. Used for __label__ + nested
 * function goto support.
 *
 * Buffer layout (10 words = 40 bytes):
 *   buf[0-7]  = r4-r11
 *   buf[8]    = SP
 *   buf[9]    = resume address (Thumb-bit set)
 */
ST_FUNC void tcc_gen_machine_nl_setjmp_mop(MachineOperand buf, MachineOperand dest)
{
  MachineCodegenContext ctx = {0};
  int buf_reg;

  if (buf.kind == MACH_OP_NONE)
  {
    buf_reg = mach_alloc_scratch(&ctx, 0);
    ot_check(th_mov_imm(buf_reg, 0, flags_safe(), ENFORCE_ENCODING_NONE));
  }
  else
  {
    buf_reg = mach_ensure_in_reg(&ctx, &buf, 0);
  }

  /* ---- save callee-saved registers r4-r11 ---- */
  ot_check_str_imm(4, buf_reg, 0, 6, ENFORCE_ENCODING_NONE);     /* r4  -> buf[0]  */
  ot_check_str_imm(5, buf_reg, 4, 6, ENFORCE_ENCODING_NONE);     /* r5  -> buf[1]  */
  ot_check_str_imm(6, buf_reg, 8, 6, ENFORCE_ENCODING_NONE);     /* r6  -> buf[2]  */
  ot_check_str_imm(R_FP, buf_reg, 12, 6, ENFORCE_ENCODING_NONE); /* r7  -> buf[3]  */
  ot_check_str_imm(8, buf_reg, 16, 6, ENFORCE_ENCODING_NONE);    /* r8  -> buf[4]  */
  ot_check_str_imm(9, buf_reg, 20, 6, ENFORCE_ENCODING_NONE);    /* r9  -> buf[5]  */
  ot_check_str_imm(10, buf_reg, 24, 6, ENFORCE_ENCODING_NONE);   /* r10 -> buf[6]  */
  ot_check_str_imm(11, buf_reg, 28, 6, ENFORCE_ENCODING_NONE);   /* r11 -> buf[7]  */

  /* ---- save SP ---- */
  ot_check_mov_reg(R_IP, R_SP, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
  ot_check_str_imm(R_IP, buf_reg, 32, 6, ENFORCE_ENCODING_NONE); /* SP -> buf[8] */

  /* ---- save resume address (ADR IP, resume_label) ---- */
  int adr_addr = ind;
  int adr_pc = adr_addr + 4;
  int adr_base = adr_pc & ~3;
  int resume_label_addr = adr_addr + 20; /* 4(ORR)+4(STR)+4(MOV)+4(B) after ADR */
  int adr_imm = resume_label_addr - adr_base;
  ot_check(th_adr_imm(R_IP, adr_imm, ENFORCE_ENCODING_32BIT));

  ot_check(th_orr_imm(R_IP, R_IP, 1, flags_safe(), ENFORCE_ENCODING_NONE)); /* Thumb bit */
  ot_check_str_imm(R_IP, buf_reg, 36, 6, ENFORCE_ENCODING_NONE);                             /* -> buf[9] */

  /* ---- normal path: return 0 ---- */
  int dest_reg = mach_get_dest_reg(&ctx, &dest, 0);
  ot_check(th_mov_imm(dest_reg, 0, flags_safe(), ENFORCE_ENCODING_32BIT)); /* dest = 0 */
  ot_check(th_b_t4(4));                                                                     /* B.W +4 (skip resume) */

  /* ---- resume_label: longjmp lands here ---- */
  ot_check(th_mov_imm(dest_reg, 1, flags_safe(), ENFORCE_ENCODING_32BIT)); /* dest = 1 */
  /* ---- end_label ---- */

  mach_writeback_dest(&dest, dest_reg);
  mach_release_all(&ctx);
}

/* __builtin_longjmp implementation for ARM Thumb-2.
 *
 * Restores FP and SP saved by __builtin_setjmp, then jumps to the resume
 * address. Uses the minimal 3-word buffer layout.
 *
 * Buffer layout (must match __builtin_setjmp):
 *   buf[0] = FP, buf[1] = resume_addr, buf[2] = SP
 */
ST_FUNC void tcc_gen_machine_longjmp_mop(MachineOperand buf)
{
  MachineCodegenContext ctx = {0};
  int buf_reg;

  if (buf.kind == MACH_OP_NONE)
  {
    tcc_error("__builtin_longjmp: invalid buffer operand");
    return;
  }

  buf_reg = mach_ensure_in_reg(&ctx, &buf, 0);

  /* Copy buf pointer to IP so it survives FP restore */
  ot_check_mov_reg(R_IP, buf_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);

  /* Read resume address and saved SP into caller-saved regs first */
  ot_check_ldr_imm(0, R_IP, 4, 6, ENFORCE_ENCODING_NONE); /* r0 = resume addr */
  ot_check_ldr_imm(1, R_IP, 8, 6, ENFORCE_ENCODING_NONE); /* r1 = saved SP    */

  /* Restore frame pointer */
  ot_check_ldr_imm(R_FP, R_IP, 0, 6, ENFORCE_ENCODING_NONE); /* r7 = FP */

  /* Restore SP */
  ot_check_mov_reg(R_SP, 1, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);

  /* Jump to resume address (Thumb bit already set by setjmp code) */
  ot_check(th_bx_reg(0));

  mach_release_all(&ctx);
}

/* Non-local goto longjmp: restores ALL callee-saved registers (r4-r11), SP,
 * then jumps to the resume address. Used for __label__ + nested function goto.
 *
 * Buffer layout (must match nl_setjmp):
 *   buf[0-7] = r4-r11, buf[8] = SP, buf[9] = resume_addr
 */
ST_FUNC void tcc_gen_machine_nl_longjmp_mop(MachineOperand buf)
{
  MachineCodegenContext ctx = {0};
  int buf_reg;

  if (buf.kind == MACH_OP_NONE)
  {
    tcc_error("nl_longjmp: invalid buffer operand");
    return;
  }

  if (buf.kind == MACH_OP_CHAIN_REL)
  {
    /* For chain-relative buffers (non-local goto from nested function),
     * we need the ADDRESS of the buffer in the parent frame, not the value.
     * mach_ensure_in_reg would load the value; use LEA logic instead. */
    ScratchRegAlloc chain_scratch = {0};
    int chain_used = 0;
    uint32_t excl = 0;
    int base = resolve_chain_base(tcc_state->ir, buf.u.chain.chain_index, excl, &chain_scratch, &chain_used);
    buf_reg = mach_alloc_scratch(&ctx, excl | (1u << (uint32_t)base));
    int32_t off = buf.u.chain.offset;
    int sign = (off < 0);
    int abs_off = sign ? (int)(-off) : (int)off;
    if (abs_off == 0)
    {
      if (buf_reg != base)
        ot_check_mov_reg((uint32_t)buf_reg, (uint32_t)base, flags_safe(), THUMB_SHIFT_DEFAULT,
                         ENFORCE_ENCODING_NONE, false);
    }
    else
    {
      thumb_opcode ins = sign
                             ? th_sub_imm(buf_reg, base, abs_off, flags_safe(), ENFORCE_ENCODING_NONE)
                             : th_add_imm(buf_reg, base, abs_off, flags_safe(), ENFORCE_ENCODING_NONE);
      if (ins.size != 0)
      {
        ot_check(ins);
      }
      else
      {
        ScratchRegAlloc off_sc = get_scratch_reg_with_save(excl | (1u << (uint32_t)buf_reg) | (1u << (uint32_t)base));
        load_full_const(off_sc.reg, PREG_NONE, LFC_SPLIT(abs_off));
        ot_check(sign ? th_sub_reg(buf_reg, base, off_sc.reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                                   ENFORCE_ENCODING_NONE)
                      : th_add_reg(buf_reg, base, off_sc.reg, flags_safe(), THUMB_SHIFT_DEFAULT,
                                   ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&off_sc);
      }
    }
    if (chain_used)
      restore_scratch_reg(&chain_scratch);
  }
  else
  {
    buf_reg = mach_ensure_in_reg(&ctx, &buf, 0);
  }

  /* Copy buf pointer to IP so it survives register restores */
  ot_check_mov_reg(R_IP, buf_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);

  /* Load resume address and saved SP into caller-saved regs first
   * (before we clobber r4+ with the restore) */
  ot_check_ldr_imm(0, R_IP, 36, 6, ENFORCE_ENCODING_NONE); /* r0 = resume addr */
  ot_check_ldr_imm(1, R_IP, 32, 6, ENFORCE_ENCODING_NONE); /* r1 = saved SP    */

  /* Restore callee-saved registers r4-r11 */
  ot_check_ldr_imm(4, R_IP, 0, 6, ENFORCE_ENCODING_NONE);     /* r4  = buf[0] */
  ot_check_ldr_imm(5, R_IP, 4, 6, ENFORCE_ENCODING_NONE);     /* r5  = buf[1] */
  ot_check_ldr_imm(6, R_IP, 8, 6, ENFORCE_ENCODING_NONE);     /* r6  = buf[2] */
  ot_check_ldr_imm(R_FP, R_IP, 12, 6, ENFORCE_ENCODING_NONE); /* r7  = buf[3] (FP) */
  ot_check_ldr_imm(8, R_IP, 16, 6, ENFORCE_ENCODING_NONE);    /* r8  = buf[4] */
  allow_r9_write = 1;
  ot_check_ldr_imm(9, R_IP, 20, 6, ENFORCE_ENCODING_NONE); /* r9  = buf[5] */
  allow_r9_write = 0;
  ot_check_ldr_imm(10, R_IP, 24, 6, ENFORCE_ENCODING_NONE); /* r10 = buf[6] */
  ot_check_ldr_imm(11, R_IP, 28, 6, ENFORCE_ENCODING_NONE); /* r11 = buf[7] */

  /* Restore SP */
  ot_check_mov_reg(R_SP, 1, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);

  /* Jump to resume address (Thumb bit already set by setjmp code) */
  ot_check(th_bx_reg(0));

  mach_release_all(&ctx);
}

/* ============================================================================
 * __builtin_apply_args / __builtin_apply implementation for ARM Thumb-2
 * ============================================================================
 *
 * __builtin_apply_args() returns a pointer to a saved argument block:
 *   [0]  pointer to incoming stack arguments (above saved register area)
 *   [4]  saved r0
 *   [8]  saved r1
 *   [12] saved r2
 *   [16] saved r3
 *
 * The prologue stores r0-r3 and the stack args pointer when
 * func_save_apply_args is set.  This handler just computes the address.
 *
 * __builtin_apply(fn, args, size) restores r0-r3 from the args block,
 * calls fn via BLX, and returns the result in dest (r0).
 */

ST_FUNC void tcc_gen_machine_builtin_apply_args_mop(MachineOperand dest)
{
  MachineCodegenContext ctx = {0};

  /* The apply_args block lives at tcc_state->apply_args_offset relative to FP.
   * Compute FP + adjusted_offset into the dest register. */
  int dest_reg = mach_get_dest_reg(&ctx, &dest, 0);
  int offset = tcc_state->apply_args_offset;
  tcc_machine_addr_of_stack_slot(dest_reg, offset, 0 /* not param */);

  mach_writeback_dest(&dest, dest_reg);
  mach_release_all(&ctx);
}

ST_FUNC void tcc_gen_machine_builtin_apply_mop(MachineOperand fn, MachineOperand args, MachineOperand dest)
{
  MachineCodegenContext ctx = {0};

  /* Step 1: Load args block pointer into a callee-saved scratch register.
   * We use the scratch allocator which will pick a suitable register. */
  int args_reg = mach_ensure_in_reg(&ctx, &args, 0);

  /* Step 2: Load the function pointer into R12 (IP), which survives the
   * register loads below because IP is not one of r0-r3. */
  int fn_reg = mach_ensure_in_reg(&ctx, &fn, (1u << args_reg));
  if (fn_reg != R_IP)
  {
    ot_check_mov_reg(R_IP, fn_reg, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
  }

  /* Step 3: Restore r0-r3 from the args block.
   * Layout: [+0]=stack_args_ptr, [+4]=r0, [+8]=r1, [+12]=r2, [+16]=r3. */
  ot_check_ldr_imm(R0, args_reg, 4, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R1, args_reg, 8, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R2, args_reg, 12, 6, ENFORCE_ENCODING_NONE);
  ot_check_ldr_imm(R3, args_reg, 16, 6, ENFORCE_ENCODING_NONE);

  /* Step 4: Call the function via BLX R12.
   * This clobbers LR and r0-r3 (caller-saved). */
  ot_check(th_blx_reg(R_IP));

  /* Step 5: Move return value (r0) to dest register. */
  int dest_reg = mach_get_dest_reg(&ctx, &dest, 0);
  if (dest_reg != R0)
  {
    ot_check_mov_reg(dest_reg, R0, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
  }

  mach_writeback_dest(&dest, dest_reg);
  mach_release_all(&ctx);
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
    /* Integer to float/double conversion.
     * Need to distinguish 32-bit int vs 64-bit long long sources:
     *  - 32-bit: __aeabi_{ui,i}2{d,f}
     *  - 64-bit: __aeabi_{ul,l}2{d,f}
     */
    int is_unsigned = (src1->type.t & VT_UNSIGNED) ? 1 : 0;
    if (src1_size == 8)
    {
      /* 64-bit integer source (long long / unsigned long long) */
      if (is_unsigned)
        return dest_64bit ? "__aeabi_ul2d" : "__aeabi_ul2f";
      return dest_64bit ? "__aeabi_l2d" : "__aeabi_l2f";
    }
    /* 32-bit integer source (int / unsigned int) */
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

/* tcc_gen_machine_func_parameter_mop: MachineOperand-based entry point for
 * FUNCPARAMVAL / FUNCPARAMVOID.  src2_enc must be MACH_OP_IMM holding the
 * packed call_id / param_idx value (same encoding as irop_get_imm64_ex).
 * src1 is the value being passed (unused here — handled by the call-site ABI).
 */
ST_FUNC void tcc_gen_machine_func_parameter_mop(MachineOperand src1, MachineOperand src2_enc, TccIrOp op)
{
  (void)src1;

  const uint32_t encoded = (uint32_t)src2_enc.u.imm.val;
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

  /* During dry-run, don't modify the argument list - it causes memory leaks
   * when we restore the call sites after dry-run. The argument list is not
   * needed for scratch register tracking anyway. */
  if (dry_run_state.active)
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
/* Emit a nested-function trampoline into the current text section.
 * chain_slot_sym: TCC symbol for the chain slot in .data
 * func_sym:       TCC symbol for the nested function in .text
 *
 * The trampoline loads the parent frame pointer from the chain slot
 * into R10 (the static-chain register) and tail-calls the nested function.
 *
 * Two variants:
 *  - GOT-indirect (text_and_data_separation): uses R9-relative GOT loads,
 *    relocations are R_ARM_GOT32 (linker-resolved, no absolute addresses
 *    in the code section).
 *  - Direct: inline literal pool with R_ARM_ABS32 relocations.
 */
ST_FUNC addr_t gen_nested_func_trampoline(Sym *chain_slot_sym, Sym *func_sym)
{
  Section *text_sec = cur_text_section;
  int use_got = tcc_state->text_and_data_separation;

  section_prealloc(text_sec, use_got ? 36 : 24);

  /* Align ind to 4-byte boundary */
  while (ind & 3)
    text_sec->data[ind++] = 0x00;

  addr_t tramp_start = ind;

  if (use_got)
  {
    /* GOT-indirect trampoline (32 bytes):
     *   +0:  LDR  r12, [pc, #20]  ; GOT offset of chain_slot (from +24)
     *   +4:  LDR  r10, [r9, r12]  ; chain_slot address via GOT
     *   +8:  LDR  r10, [r10, #0]  ; *chain_slot = parent FP
     *   +12: LDR  r12, [pc, #12]  ; GOT offset of function (from +28)
     *   +16: LDR  r12, [r9, r12]  ; function address via GOT
     *   +20: BX   r12             ; tail-call
     *   +22: NOP
     *   +24: .word 0              ; R_ARM_GOT32 chain_slot
     *   +28: .word 0              ; R_ARM_GOT32 function
     */

    /* +0: LDR R12, [PC, #20] - F8DF C014 */
    text_sec->data[ind++] = 0xDF;
    text_sec->data[ind++] = 0xF8;
    text_sec->data[ind++] = 0x14;
    text_sec->data[ind++] = 0xC0;

    /* +4: LDR R10, [R9, R12] - F859 A00C */
    text_sec->data[ind++] = 0x59;
    text_sec->data[ind++] = 0xF8;
    text_sec->data[ind++] = 0x0C;
    text_sec->data[ind++] = 0xA0;

    /* +8: LDR R10, [R10, #0] - F8DA A000 */
    text_sec->data[ind++] = 0xDA;
    text_sec->data[ind++] = 0xF8;
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0xA0;

    /* +12: LDR R12, [PC, #12] - F8DF C00C */
    text_sec->data[ind++] = 0xDF;
    text_sec->data[ind++] = 0xF8;
    text_sec->data[ind++] = 0x0C;
    text_sec->data[ind++] = 0xC0;

    /* +16: LDR R12, [R9, R12] - F859 C00C */
    text_sec->data[ind++] = 0x59;
    text_sec->data[ind++] = 0xF8;
    text_sec->data[ind++] = 0x0C;
    text_sec->data[ind++] = 0xC0;

    /* +20: BX R12 - 4760 */
    text_sec->data[ind++] = 0x60;
    text_sec->data[ind++] = 0x47;

    /* +22: NOP - BF00 */
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0xBF;

    /* +24: chain slot GOT offset */
    greloc(text_sec, chain_slot_sym, ind, R_ARM_GOT32);
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;

    /* +28: function GOT offset */
    greloc(text_sec, func_sym, ind, R_ARM_GOT32);
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;
  }
  else
  {
    /* Direct trampoline (20 bytes):
     *   +0:  LDR  r10, [pc, #8]   ; chain_slot address (from +12)
     *   +4:  LDR  r10, [r10, #0]  ; *chain_slot = parent FP
     *   +8:  LDR  pc, [pc, #4]    ; function address (from +16), tail call
     *   +12: .word chain_slot     ; R_ARM_ABS32
     *   +16: .word function        ; R_ARM_ABS32
     */

    /* LDR R10, [PC, #8] - F8DF A008 */
    text_sec->data[ind++] = 0xDF;
    text_sec->data[ind++] = 0xF8;
    text_sec->data[ind++] = 0x08;
    text_sec->data[ind++] = 0xA0;

    /* LDR R10, [R10, #0] - F8DA A000 */
    text_sec->data[ind++] = 0xDA;
    text_sec->data[ind++] = 0xF8;
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0xA0;

    /* LDR PC, [PC, #4] - F8DF F004 */
    text_sec->data[ind++] = 0xDF;
    text_sec->data[ind++] = 0xF8;
    text_sec->data[ind++] = 0x04;
    text_sec->data[ind++] = 0xF0;

    /* chain slot address */
    greloc(text_sec, chain_slot_sym, ind, R_ARM_ABS32);
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;

    /* function address */
    greloc(text_sec, func_sym, ind, R_ARM_ABS32);
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;
    text_sec->data[ind++] = 0x00;
  }

  text_sec->data_offset = ind;
  return tramp_start + 1; /* +1 for Thumb interworking bit */
}
