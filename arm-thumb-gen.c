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
#include "ir/opt.h"
#include "tcc.h"
#include "tccir.h"
#include "tccls.h"
#include "tcctype.h"

static void load_full_const(int r, int r1, int64_t imm, struct Sym *sym);

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
  {
    fprintf(stderr, "[TCC-DIAG] validate_sym_for_reloc: sym->v=0x%x has SYM_FIELD, c=%d\n", sym->v, sym->c);
    return NULL;
  }
  /* Symbols with c < 0 are not properly registered */
  if (sym->c < 0)
  {
    const char *name = get_tok_str(sym->v & ~SYM_FIELD, NULL);
    fprintf(stderr, "[TCC-DIAG] validate_sym_for_reloc: sym '%s' has c=%d (<0)\n", name ? name : "?", sym->c);
    return NULL;
  }
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
unsigned char pic;

int offset_to_args = 0;

thumb_flags_behaviour g_setflags = FLAGS_BEHAVIOUR_SET;

uint32_t caller_saved_registers;
uint32_t pushed_registers;
int allocated_stack_size;
int callee_push_size = 0;       /* bytes pushed BELOW FP in two-phase push */
uint32_t callee_saved_regs = 0; /* register mask for second push (below FP) */
int vararg_push_size = 0;       /* bytes pushed for variadic r0-r3 save (16 or 0) */

/* Adjust a local/spill frame offset when two-phase push is active and
 * callee-saved regs are pushed below FP.  Only adjusts negative non-param
 * offsets (locals/spills); positive and param offsets are unchanged. */
static inline int fp_adjust_local_offset(int frame_offset, int is_param)
{
  if (!is_param && frame_offset < 0 && callee_push_size > 0)
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
static int scratch_push_count = 0;

/* Debug tracking: current IR opcode being processed (set by codegen.c) */
int g_debug_current_op = -1;

int is_valid_opcode(thumb_opcode op);
int ot(thumb_opcode op);
int ot_check(thumb_opcode op);
static void thumb_require_materialized_reg(const char *ctx, const char *operand, int reg);
static bool thumb_is_hw_reg(int reg);
static int get_struct_base_addr_mop(const MachineOperand *mop, int default_reg);
int th_has_immediate_value(int r);
int load_word_from_base(int ir, int base, int fc, int sign);
int th_patch_call(int t, int a);
/* Structure to track scratch register allocation with potential save/restore */
typedef struct ScratchRegAlloc
{
  int reg : 30;            /* The allocated scratch register (range 0-15 for ARM) */
  uint32_t saved : 1;      /* Whether the register was pushed to stack (real emit only) */
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
  ot_check(th_mov_reg(out_scratch->reg, architecture_config.static_chain_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, no_shift,
                      ENFORCE_ENCODING_NONE, false));

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
    int r = mach_alloc_scratch(ctx, excl);
    Sym *sym = op->u.sym.sym ? validate_sym_for_reloc(op->u.sym.sym) : NULL;
    if (!op->needs_deref)
    {
      /* Load symbol address (with addend baked in). */
      tcc_machine_load_constant(r, PREG_REG_NONE, op->u.sym.addend, 0, sym);
    }
    else
    {
      /* Load symbol address into a scratch base reg, then dereference. */
      int base = mach_alloc_scratch(ctx, excl | (1u << (uint32_t)r));
      tcc_machine_load_constant(base, PREG_REG_NONE, 0, 0, sym);
      const int32_t addend = op->u.sym.addend;
      load_from_base(r, PREG_REG_NONE, op->btype, (int)op->is_unsigned, addend < 0 ? (int)(-addend) : (int)addend,
                     addend < 0 ? 1 : 0, (uint32_t)base);
    }
    return r;
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
    if (ot(imm_handler((uint32_t)dest_reg, (uint32_t)src1_reg, imm_val, flags, ENFORCE_ENCODING_NONE)))
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
        ot_check(th_mov_reg((uint32_t)op->u.reg.r0, (uint32_t)reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE, false));
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
        ot_check(th_mov_reg((uint32_t)dest_reg, (uint32_t)op->u.reg.r0, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                            THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
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
      ot_check(th_mov_reg((uint32_t)dest_reg, (uint32_t)r, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE, false));
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

/* Hash table for O(1) literal pool lookups instead of O(n) linear search.
 * Key: (sym, imm), Value: index into literal pool array.
 * Using open addressing with linear probing. */
#define LITERAL_POOL_HASH_SIZE 256 /* Power of 2 for fast modulo */
typedef struct LiteralPoolHashEntry
{
  Sym *sym;
  int64_t imm;
  int pool_index; /* Index into literal pool array, or -1 if empty */
  int valid;      /* 1 if this slot contains a valid entry, 0 if empty */
} LiteralPoolHashEntry;

static LiteralPoolHashEntry literal_pool_hash[LITERAL_POOL_HASH_SIZE];
static LiteralPoolHashEntry dry_run_literal_pool_hash[LITERAL_POOL_HASH_SIZE];

static inline uint32_t literal_pool_hash_func(Sym *sym, int64_t imm)
{
  /* Simple hash combining pointer and immediate value */
  uint64_t h = (uint64_t)(uintptr_t)sym;
  h ^= (uint64_t)imm;
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdULL;
  h ^= h >> 33;
  return (uint32_t)(h & (LITERAL_POOL_HASH_SIZE - 1));
}

static void literal_pool_hash_clear(LiteralPoolHashEntry *hash)
{
  for (int i = 0; i < LITERAL_POOL_HASH_SIZE; i++)
  {
    hash[i].valid = 0;
    hash[i].pool_index = -1;
  }
}

static int literal_pool_hash_find(LiteralPoolHashEntry *hash, Sym *sym, int64_t imm)
{
  uint32_t idx = literal_pool_hash_func(sym, imm);
  for (int i = 0; i < LITERAL_POOL_HASH_SIZE; i++)
  {
    uint32_t probe = (idx + i) & (LITERAL_POOL_HASH_SIZE - 1);
    if (!hash[probe].valid)
    {
      return -1; /* Empty slot - not found */
    }
    if (hash[probe].sym == sym && hash[probe].imm == imm)
    {
      return hash[probe].pool_index;
    }
  }
  return -1; /* Table full, not found */
}

static void literal_pool_hash_insert(LiteralPoolHashEntry *hash, Sym *sym, int64_t imm, int pool_index)
{
  uint32_t idx = literal_pool_hash_func(sym, imm);
  for (int i = 0; i < LITERAL_POOL_HASH_SIZE; i++)
  {
    uint32_t probe = (idx + i) & (LITERAL_POOL_HASH_SIZE - 1);
    if (!hash[probe].valid)
    {
      hash[probe].sym = sym;
      hash[probe].imm = imm;
      hash[probe].pool_index = pool_index;
      hash[probe].valid = 1;
      return;
    }
  }
  /* Table full - this shouldn't happen with reasonable pool sizes */
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
  branch_opt_state.optimization_enabled = 0; /* Disabled: dry-run addresses diverge from real pass */
  branch_opt_state.code_size_reduction = 0;
  if (!branch_opt_state.branches)
  {
    branch_opt_state.branch_capacity = 64;
    branch_opt_state.branches = tcc_malloc(branch_opt_state.branch_capacity * sizeof(BranchInfo));
  }
}

/* Record a branch for later optimization analysis */
static void branch_opt_record(int ir_index, int source_addr, int target_ir, int is_conditional)
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

#ifdef DEBUG_BRANCH_OPT
  fprintf(stderr,
          "[BRANCH_OPT] %d branches, %d converted to 16-bit, "
          "%d bytes saved, %d iterations\n",
          branch_opt_state.branch_count, branch_opt_state.code_size_reduction / 2, branch_opt_state.code_size_reduction,
          iterations);
#endif
}

/* Lookup encoding decision for a given IR index */
/* Local version that returns the enum type */
static BranchEncoding branch_opt_get_encoding(int ir_index)
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
  /* Clear the dry-run hash table */
  literal_pool_hash_clear(dry_run_literal_pool_hash);
  /* Save thumb_gen_state before dry-run */
  thumb_gen_state_snapshot_save(&dry_run_snapshot);
  /* Reset state that should start fresh for dry-run */
  thumb_gen_state.code_size = 0;
  thumb_gen_state.literal_pool_count = 0;
  thumb_gen_state.cached_global_sym = NULL;
  thumb_gen_state.cached_global_reg = PREG_NONE;
  thumb_gen_state.function_argument_count = 0;
  /* call_sites_by_id - don't modify, just track that we saved it */
}

ST_FUNC void tcc_gen_machine_dry_run_end(void)
{
  dry_run_state.active = 0;
  /* Restore thumb_gen_state after dry-run */
  thumb_gen_state_snapshot_restore(&dry_run_snapshot);
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
  memset(scratch_push_stack, 0, sizeof(scratch_push_stack));
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

  /* No free register found - we need to save one to the stack */
  /* Prefer R_IP (R12) as it's the inter-procedure scratch register */
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

  ot_check(th_push(1 << reg_to_save));
  result.reg = reg_to_save;
  result.saved = 1;
  result.would_save = 1; /* Phase 3: push was needed */
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
  /* Dry run: don't emit pops, just reset tracking */
  if (dry_run_state.active)
  {
    scratch_push_count = 0;
    scratch_global_exclude = text_and_data_separation ? (1u << R9) : 0;
    return;
  }

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
    fprintf(stderr, "[ot_check FAIL] opcode=0x%x ind=0x%x ir_op=%d\n", op.opcode, (unsigned)ind, g_debug_current_op);
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
  thumb_gen_state.generating_function = 0;
  thumb_gen_state.code_size = 0;
  thumb_gen_state.cached_global_sym = NULL;
  thumb_gen_state.cached_global_reg = PREG_NONE;
  /* Clear the hash table for O(1) lookups */
  literal_pool_hash_clear(literal_pool_hash);
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
  if (s->fpu_type == 0 || s->fpu_type == ARM_FPU_NONE)
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
    thumb_opcode nop =
        th_mov_reg(R0, R0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
    o(nop.opcode & 0xffff);
  }

  /* Array to store the output position of each unique literal */
  int *literal_positions = tcc_malloc(pool_count * sizeof(int));

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
  literal_pool_hash_clear(literal_pool_hash);
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
static int thumb_decode_dest_reg(thumb_opcode op)
{
  uint32_t w = op.opcode;

  if (op.size == 2)
  {
    uint16_t hw = (uint16_t)(w & 0xFFFF);
    /* 16-bit MOV (high registers): 0100 0110 D Rm4 Rd3
     * Bits [15:8]=0x46, D=bit7 of lower byte, Rd3=bits[2:0] */
    if ((hw >> 8) == 0x46)
      return ((hw >> 4) & 0x08) | (hw & 0x07);
    /* 16-bit ADD (high registers): 0100 0100 D Rm4 Rd3 */
    if ((hw >> 8) == 0x44)
      return ((hw >> 4) & 0x08) | (hw & 0x07);
    /* 16-bit CMP (high registers): 0100 0101 — no dest write, skip */
    /* Low-register forms (R0-R7 only) can't reach R9 */
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
  }

  return -1;
}

int ot(thumb_opcode op)
{
  if (op.size == 0)
    return op.size;

  /* Detect instructions that write to R9 when it's reserved for GOT pointer.
   * Exclude push/pop/stmdb/ldmia which legitimately save/restore R9. */
  if (text_and_data_separation)
  {
    int dest = thumb_decode_dest_reg(op);
    if (dest == R9)
    {
      tcc_error("instruction 0x%0*x (size=%d) writes to R9 (GOT pointer) at ind=0x%x ir_op=%d", op.size == 4 ? 8 : 4,
                op.opcode, op.size, (unsigned)ind, g_debug_current_op);
    }
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

ST_FUNC void tcc_gen_machine_indirect_jump_mop(MachineOperand src, TccIrOp op)
{
  (void)op;
  MachineCodegenContext ctx = {0};
  int target = mach_ensure_in_reg(&ctx, &src, 0);
  ot_check(th_bx_reg((uint16_t)target));
  mach_release_all(&ctx);
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

  /* Reuse index_reg as scratch - it's dead after SWITCH_TABLE (terminator). */
  int rt = index_reg;

  ot_check(th_lsl_imm(rt, index_reg, 2, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_32BIT));
  ot_check(th_add_reg(rt, rt, R_PC, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
  ot_check(th_ldr_imm(rt, rt, 6, 6, ENFORCE_ENCODING_32BIT));
  ot_check(th_add_reg(rt, rt, R_PC, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
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

  /* Store SP to the local stack slot at frame offset `addr`. */
  int off = fp_adjust_local_offset(addr, 0 /* not param */);
  int sign = (off < 0) ? 1 : 0;
  int abs_off = sign ? -off : off;

  ot_check(th_mov_reg(R_IP, R_SP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
  th_store32_imm_or_reg_ex(R_IP, R_FP, abs_off, sign, 0);
}

ST_FUNC void gen_vla_sp_restore(int addr)
{
  if (nocode_wanted)
    return;

  /* Load SP from the local stack slot at frame offset `addr`. */
  int off = fp_adjust_local_offset(addr, 0 /* not param */);
  int sign = (off < 0) ? 1 : 0;
  int abs_off = sign ? -off : off;

  load_from_base(R_IP, PREG_REG_NONE, IROP_BTYPE_INT32, 0, abs_off, sign, R_FP);
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
    }
    entry = &dry_run_literal_pool[dry_run_literal_pool_count++];
    memset(entry, 0, sizeof(ThumbLiteralPoolEntry));
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
  }
  entry = &thumb_gen_state.literal_pool[thumb_gen_state.literal_pool_count++];
  memset(entry, 0, sizeof(ThumbLiteralPoolEntry));
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
  LiteralPoolHashEntry *hash;
  int new_index;

  if (dry_run_state.active)
  {
    hash = dry_run_literal_pool_hash;
    new_index = dry_run_literal_pool_count;
  }
  else
  {
    hash = literal_pool_hash;
    new_index = thumb_gen_state.literal_pool_count;
  }

  /* O(1) hash lookup instead of O(n) linear search */
  found_index = literal_pool_hash_find(hash, sym, imm);

  /* Allocate new entry */
  ThumbLiteralPoolEntry *entry = th_literal_pool_allocate();
  if (found_index >= 0)
  {
    /* Mark as sharing with the found entry */
    entry->shared_index = found_index;
  }
  else
  {
    /* This is a new primary entry - add to hash table */
    literal_pool_hash_insert(hash, sym, imm, new_index);
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
        const char *name = get_tok_str(sym->v & ~SYM_FIELD, NULL);
        fprintf(stderr, "[TCC-DIAG] load_full_const: put_extern_sym failed for '%s', c=%d\n", name ? name : "?",
                sym->c);
        sym = NULL;
      }
    }

    if (sym)
    {
      esym = elfsym(sym);
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
    load_ins = th_ldrd_imm(r, r1, R_PC, 0, 4, ENFORCE_ENCODING_NONE);
  }
  ot_check(load_ins);
  patch_pos = ind - load_ins.size;

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

  /* Re-derive esym after ot_check(): literal pool generation during ot_check
   * can call put_elf_sym → section_ptr_add → section_realloc, which may
   * free and reallocate the symtab section buffer, invalidating any
   * earlier ElfSym pointer. */
  if (sym)
    esym = elfsym(sym);
  if (esym)
  {
    sym_off = esym->st_shndx;
  }
  if (!pic)
  {
    if (sym)
    {
      entry->relocation = R_ARM_ABS32;
      /* The imm value is the addend (offset from symbol base).
         For arr[i], imm = i * sizeof(element).
         The linker will add the symbol's address to this addend. */
      entry->imm = imm;
    }
    else
    {
      entry->imm = imm;
    }
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
        if (sym->type.t & VT_STATIC && sym_off != cur_text_section->sh_num && !sym_in_code_section)
        {
          /* Static data symbol — GOTOFF (same segment as GOT) */
          entry->relocation = R_ARM_GOTOFF;
        }
        else
        {
          if (sym->type.t & VT_STATIC && sym_off != cur_text_section->sh_num && sym_in_code_section)
          {
            const char *sym_name = get_tok_str(sym->v & ~SYM_FIELD, NULL);
            fprintf(stderr, "[TCC] static code sym '%s' in sec %d (cur %d) -> GOT32\n", sym_name ? sym_name : "?",
                    sym_off, cur_text_section->sh_num);
          }
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
        if (sym->type.t & VT_STATIC && sym_off != cur_text_section->sh_num && !sym_in_code_section_cg)
        {
          /* Static data symbol — GOTOFF (add R9) */
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
      ot_check(th_mov_reg(dest_reg, base_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                          false));
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
        ot_check(th_mov_reg(dest_reg, cached_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE, false));
      }
      return;
    }
    /* Cached in scratch register - don't use it */
  }

  const int neg = (frame_offset < 0);
  int abs_off = neg ? -frame_offset : frame_offset;
  thumb_opcode op = neg ? th_sub_imm(dest_reg, base_reg, abs_off, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)
                        : th_add_imm(dest_reg, base_reg, abs_off, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);

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

  load_full_const(offset_reg, PREG_NONE, frame_offset, NULL);
  ot_check(th_add_reg(dest_reg, base_reg, offset_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
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
      load_full_const(dest_reg, dest_reg_high, value, validated_sym);
      return;
    }
    /* Invalid or missing sym - fall through to treat as plain constant */
    {
      const char *name = get_tok_str(sym->v & ~SYM_FIELD, NULL);
      fprintf(stderr, "[TCC-DIAG] tcc_machine_load_constant: sym '%s' failed validation, loading plain value=%lld\n",
              name ? name : "?", (long long)value);
    }
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
    ot_check(th_mov_imm((uint32_t)rn_hi, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }
  if (thumb_is_hw_reg(rn_hi))
    excl |= (1u << (uint32_t)rn_hi);

  /* 3. Load src2 and emit the 64-bit operation. */
  const thumb_flags_behaviour lo_flags = uses_carry ? FLAGS_BEHAVIOUR_SET : FLAGS_BEHAVIOUR_NOT_IMPORTANT;
  if (src2->kind == MACH_OP_IMM)
  {
    const uint32_t imm_lo = (uint32_t)((uint64_t)src2->u.imm.val & 0xffffffffu);
    const uint32_t imm_hi = (uint32_t)((uint64_t)src2->u.imm.val >> 32);
    thumb_emit_op_imm_fallback(rd_lo, rn_lo, imm_lo, lo_flags, regular);
    thumb_emit_op_imm_fallback(rd_hi, rn_hi, imm_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, carry_h);
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
      ot_check(th_mov_imm((uint32_t)rm_hi, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    }
    ot_check(regular.reg_handler((uint32_t)rd_lo, (uint32_t)rn_lo, (uint32_t)rm_lo, lo_flags, THUMB_SHIFT_DEFAULT,
                                 ENFORCE_ENCODING_NONE));
    ot_check(carry_h.reg_handler((uint32_t)rd_hi, (uint32_t)rn_hi, (uint32_t)rm_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                                 THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
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
                                   TccIrOp op)
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

  /* Load src1 high half or compute by extension. */
  int src_hi;
  if (src1->is_64bit)
  {
    MachineOperand s1_hi = mach_make_hi_half(src1);
    src_hi = mach_ensure_in_reg(&mctx, &s1_hi, excl);
    if (thumb_is_hw_reg(src_hi))
      excl |= (1u << (uint32_t)src_hi);
  }
  else
  {
    src_hi = mach_alloc_scratch(&mctx, excl);
    excl |= (1u << (uint32_t)src_hi);
    if (arith_right)
      ot_check(
          th_asr_imm((uint32_t)src_hi, (uint32_t)src_lo, 31, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    else
      ot_check(th_mov_imm((uint32_t)src_hi, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }

  /* Emit the shift — logic identical to thumb_emit_shift64_imm core. */
  if (sh == 0)
  {
    ot_check(th_mov_reg((uint32_t)dst_lo, (uint32_t)src_lo, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                        ENFORCE_ENCODING_NONE, false));
    ot_check(th_mov_reg((uint32_t)dst_hi, (uint32_t)src_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                        ENFORCE_ENCODING_NONE, false));
  }
  else if (sh < 32)
  {
    const int regs[] = {dst_lo, dst_hi, src_lo, src_hi};
    ScratchRegAlloc tmp = get_scratch_reg_with_save(thumb_exclude_mask_for_regs(4, regs) | excl);
    if (is_left)
    {
      ot_check(
          dst_lo_shift((uint32_t)dst_lo, (uint32_t)src_lo, sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      ot_check(cross_shift((uint32_t)tmp.reg, (uint32_t)src_lo, 32 - sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                           ENFORCE_ENCODING_NONE));
      ot_check(
          dst_hi_shift((uint32_t)dst_hi, (uint32_t)src_hi, sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      ot_check(th_orr_reg((uint32_t)dst_hi, (uint32_t)dst_hi, (uint32_t)tmp.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                          THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    }
    else
    {
      ot_check(cross_shift((uint32_t)tmp.reg, (uint32_t)src_hi, 32 - sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                           ENFORCE_ENCODING_NONE));
      ot_check(
          th_lsr_imm((uint32_t)dst_lo, (uint32_t)src_lo, sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      ot_check(th_orr_reg((uint32_t)dst_lo, (uint32_t)dst_lo, (uint32_t)tmp.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                          THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      ot_check(
          dst_hi_shift((uint32_t)dst_hi, (uint32_t)src_hi, sh, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    }
    restore_scratch_reg(&tmp);
  }
  else if (sh == 32)
  {
    if (is_left)
    {
      ot_check(th_mov_imm((uint32_t)dst_lo, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      ot_check(th_mov_reg((uint32_t)dst_hi, (uint32_t)src_lo, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE, false));
    }
    else
    {
      ot_check(th_mov_reg((uint32_t)dst_lo, (uint32_t)src_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE, false));
      if (arith_right)
        ot_check(
            th_asr_imm((uint32_t)dst_hi, (uint32_t)src_hi, 31, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      else
        ot_check(th_mov_imm((uint32_t)dst_hi, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    }
  }
  else if (sh < 64)
  {
    if (is_left)
    {
      ot_check(th_mov_imm((uint32_t)dst_lo, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      ot_check(dst_hi_shift((uint32_t)dst_hi, (uint32_t)src_lo, sh - 32, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                            ENFORCE_ENCODING_NONE));
    }
    else
    {
      ot_check(dst_hi_shift((uint32_t)dst_lo, (uint32_t)src_hi, sh - 32, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                            ENFORCE_ENCODING_NONE));
      if (arith_right)
        ot_check(
            th_asr_imm((uint32_t)dst_hi, (uint32_t)src_hi, 31, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      else
        ot_check(th_mov_imm((uint32_t)dst_hi, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    }
  }
  else /* sh >= 64 */
  {
    if (is_left)
    {
      ot_check(th_mov_imm((uint32_t)dst_lo, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      ot_check(th_mov_imm((uint32_t)dst_hi, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    }
    else if (arith_right)
    {
      ot_check(
          th_asr_imm((uint32_t)dst_hi, (uint32_t)src_hi, 31, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      ot_check(th_mov_reg((uint32_t)dst_lo, (uint32_t)dst_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE, false));
    }
    else
    {
      ot_check(th_mov_imm((uint32_t)dst_lo, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
      ot_check(th_mov_imm((uint32_t)dst_hi, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    }
  }

  /* Write back. */
  if (store_lo)
  {
    MachineOperand dst_lo_op = mach_make_lo_half(dest);
    dst_lo_op.btype = IROP_BTYPE_INT32;
    mach_writeback_dest(&dst_lo_op, dst_lo);
  }
  if (store_hi)
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
                                             thumb_flags_behaviour flags)
{
  const bool dest_sets_flags = (op == TCCIR_OP_CMP);
  MachineCodegenContext mctx = {0};

  /* 1. Determine dest register (allocate scratch for spills/param/no-reg).
   * CMP and other flag-setting ops don't write a result register, so we
   * use R0 as a dummy (Rd field is architecturally ignored). */
  int dest_reg;
  if (dest_sets_flags)
    dest_reg = R0;
  else
    dest_reg = mach_get_dest_reg(&mctx, dest, 0);

  uint32_t excl = thumb_is_hw_reg(dest_reg) ? (1u << (uint32_t)dest_reg) : 0;

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
    /* Immediate form didn't fit (or src2 isn't an immediate): emit reg form. */
    ot_check(handler.reg_handler((uint32_t)dest_reg, (uint32_t)src1_reg, (uint32_t)src2_reg, flags, THUMB_SHIFT_DEFAULT,
                                 ENFORCE_ENCODING_NONE));
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
void tcc_gen_machine_data_processing_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op)
{
  ThumbDataProcessingHandler handler;
  ThumbDataProcessingHandler carry_handler; /* used for hi word of 64-bit ops */
  bool uses_carry = false;
  thumb_flags_behaviour flags = FLAGS_BEHAVIOUR_NOT_IMPORTANT;

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
    handler.imm_handler = th_cmp_imm;
    handler.reg_handler = th_cmp_reg;
    carry_handler = handler;
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

  /* Dispatch 64-bit pair destinations to the mop64 path. */
  if (dest.is_64bit)
  {
    if (op == TCCIR_OP_SHL || op == TCCIR_OP_SHR || op == TCCIR_OP_SAR)
      thumb_emit_shift64_mop(&src1, &src2, &dest, op);
    else
      thumb_emit_data_processing_mop64(&src1, &src2, &dest, op, handler, carry_handler, uses_carry);
    return;
  }

  thumb_emit_data_processing_mop32(&src1, &src2, &dest, op, handler, flags);
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
  ot_check(th_sub_reg(dest_reg, src1_reg, quotient_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
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
ST_FUNC void tcc_gen_machine_muldiv_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest, TccIrOp op)
{
  MachineCodegenContext ctx = {0};
  switch (op)
  {
  case TCCIR_OP_MUL:
    if (src1.is_64bit || src2.is_64bit || dest.is_64bit)
      thumb_emit_mul64_mop(&ctx, &src1, &src2, &dest);
    else
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
      ot_check(th_cmp_imm(0, r_lo, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
      th_literal_pool_reserve_upcoming_bytes(6);
      ot_check(th_it(mapcc(TOK_EQ), 0x8)); /* IT EQ (single instruction) */
      ot_check(th_cmp_imm(0, r_hi, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
    }
    else
    {
      /* 32-bit: CMP src, #0 — no destination, only flags. */
      int src_reg = mach_ensure_in_reg(&ctx, &src1, 0);
      ot_check(th_cmp_imm(0, src_reg, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
    }
    break;
  }
  default:
    tcc_error("compiler_error: tcc_gen_machine_muldiv_mop: unhandled op %d", (int)op);
    break;
  }
  mach_release_all(&ctx);
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

  int src1_reg = mach_ensure_in_reg(&ctx, &src1, 0);
  uint32_t excl = thumb_is_hw_reg(src1_reg) ? (1u << (uint32_t)src1_reg) : 0u;

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
      ot_check(th_mov_reg((uint32_t)dest_reg, (uint32_t)src_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE, false));
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
 *   MOV dest, #0
 *   IT  <cond>
 *   MOV dest, #1
 *
 * 64-bit dest pair (e.g. long long result = (x > y)):
 *   The boolean result 0 or 1 fits in 32 bits, so hi word is always 0.
 *   MOV dest_lo, #0
 *   IT  <cond>
 *   MOV dest_lo, #1
 *   MOV dest_hi, #0   (unconditional, outside IT block — hi is always 0)
 */
ST_FUNC void tcc_gen_machine_setif_mop(MachineOperand src, MachineOperand dest, TccIrOp op)
{
  (void)op;
  MachineCodegenContext mctx = {0};

  const int cond = mapcc((int)src.u.imm.val);

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

    /* Emit SETIF sequence for lo word. */
    ot_check(th_mov_imm(lo_reg, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE));
    th_literal_pool_reserve_upcoming_bytes(6);
    ot_check(th_it(cond, 0x8)); /* IT <cond> — single conditioned instruction */
    ot_check(th_mov_imm(lo_reg, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    /* Hi word is always 0 — boolean result never exceeds 1 (i.e. fits in 32-bit lo). */
    ot_check(th_mov_imm(hi_reg, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE));

    mach_writeback_dest(&dst_lo, lo_reg);
    mach_writeback_dest(&dst_hi, hi_reg);
  }
  else
  {
    int dest_reg = mach_get_dest_reg(&mctx, &dest, 0);

    ot_check(th_mov_imm(dest_reg, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE));
    th_literal_pool_reserve_upcoming_bytes(6);
    ot_check(th_it(cond, 0x8)); /* IT <cond> — single conditioned instruction */
    ot_check(th_mov_imm(dest_reg, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

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
      ot_check(th_orr_reg(r1, r1, hi1_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
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
      ot_check(th_orr_reg(r2, r2, hi2_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
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
      ot_check(th_mov_imm(dest_reg, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
    }
    else /* TCCIR_OP_BOOL_AND */
    {
      ot_check(th_cmp_imm(0, r1, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
      th_literal_pool_reserve_upcoming_bytes(6);
      ot_check(th_it(0x1, 0x8));                                                  /* IT NE */
      ot_check(th_cmp_imm(0, r2, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE)); /* CMPne r2, #0 */
      ot_check(th_mov_imm(dest_reg, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE));
      th_literal_pool_reserve_upcoming_bytes(6);
      ot_check(th_it(0x1, 0x8)); /* IT NE */
      ot_check(th_mov_imm(dest_reg, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
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
    ot_check(th_mov_imm(dest_reg, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }
  else /* TCCIR_OP_BOOL_AND */
  {
    ot_check(th_cmp_imm(0, src1_reg, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE));
    th_literal_pool_reserve_upcoming_bytes(6);
    ot_check(th_it(0x1, 0x8));                                                        /* IT NE */
    ot_check(th_cmp_imm(0, src2_reg, 0, FLAGS_BEHAVIOUR_SET, ENFORCE_ENCODING_NONE)); /* CMPne src2, #0 */
    ot_check(th_mov_imm(dest_reg, 0, FLAGS_BEHAVIOUR_BLOCK, ENFORCE_ENCODING_NONE));
    th_literal_pool_reserve_upcoming_bytes(6);
    ot_check(th_it(0x1, 0x8)); /* IT NE */
    ot_check(th_mov_imm(dest_reg, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
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
        ot_check(th_mov_reg((uint32_t)dest_reg, (uint32_t)src.u.reg.r0, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                            THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
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
        ot_check(th_mov_reg((uint32_t)dest_r1, (uint32_t)src.u.reg.r1, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                            THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
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
    load_from_base(dest_reg, dest_r1, btype, is_unsigned, addend < 0 ? (int)(-addend) : (int)addend, addend < 0 ? 1 : 0,
                   (uint32_t)addr_r);
    break;
  }

  case MACH_OP_IMM:
    /* Treat as constant load (e.g. loading from address 0 — rare but handle gracefully) */
    tcc_machine_load_constant(dest_reg, dest_r1, src.u.imm.val, (int)dest.is_64bit, NULL);
    break;

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

    const int lo_reg = mach_ensure_in_reg(&ctx, &src_lo, 0);
    uint32_t excl = thumb_is_hw_reg(lo_reg) ? (1u << (uint32_t)lo_reg) : 0u;
    const int hi_reg = mach_ensure_in_reg(&ctx, &src_hi, excl);
    excl |= thumb_is_hw_reg(hi_reg) ? (1u << (uint32_t)hi_reg) : 0u;

    switch (dest.kind)
    {
    case MACH_OP_REG:
      if (dest.needs_deref)
      {
        /* 64-bit pointer-store: STR lo, [base]; STR hi, [base, #4] */
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
            ot_check(th_mov_reg((uint32_t)dreg_lo, (uint32_t)lo_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                ENFORCE_ENCODING_NONE, false));
          if (dreg_hi != hi_reg && dreg_hi != (int)PREG_REG_NONE)
            ot_check(th_mov_reg((uint32_t)dreg_hi, (uint32_t)hi_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                ENFORCE_ENCODING_NONE, false));
        }
        else
        {
          if (dreg_hi != hi_reg && dreg_hi != (int)PREG_REG_NONE)
            ot_check(th_mov_reg((uint32_t)dreg_hi, (uint32_t)hi_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                ENFORCE_ENCODING_NONE, false));
          if (dreg_lo != lo_reg && dreg_lo != (int)PREG_REG_NONE)
            ot_check(th_mov_reg((uint32_t)dreg_lo, (uint32_t)lo_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                ENFORCE_ENCODING_NONE, false));
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
        th_store32_imm_or_reg_ex(lo_reg, (uint32_t)ptr_r, 0, 0, excl | (1u << (uint32_t)ptr_r));
        th_store32_imm_or_reg_ex(hi_reg, (uint32_t)ptr_r, 4, 0, excl | (1u << (uint32_t)ptr_r));
      }
      else
      {
        const int adj_hi = adj + 4;
        th_store32_imm_or_reg_ex(lo_reg, base, adj < 0 ? -adj : adj, adj < 0 ? 1 : 0, excl | (1u << base));
        th_store32_imm_or_reg_ex(hi_reg, base, adj_hi < 0 ? -adj_hi : adj_hi, adj_hi < 0 ? 1 : 0, excl | (1u << base));
      }
      break;
    }

    case MACH_OP_PARAM_STACK:
    {
      const int adj = dest.u.param.offset + offset_to_args;
      const int adj_hi = adj + 4;
      const uint32_t base = (uint32_t)(tcc_state->need_frame_pointer ? R_FP : R_SP);
      th_store32_imm_or_reg_ex(lo_reg, base, adj < 0 ? -adj : adj, adj < 0 ? 1 : 0, excl | (1u << base));
      th_store32_imm_or_reg_ex(hi_reg, base, adj_hi < 0 ? -adj_hi : adj_hi, adj_hi < 0 ? 1 : 0, excl | (1u << base));
      break;
    }

    case MACH_OP_SYMBOL:
    {
      Sym *sym = dest.u.sym.sym ? validate_sym_for_reloc(dest.u.sym.sym) : NULL;
      int addr_r = mach_alloc_scratch(&ctx, excl);
      tcc_machine_load_constant(addr_r, PREG_REG_NONE, 0, 0, sym);
      const int32_t addend = dest.u.sym.addend;
      const int32_t addend_hi = addend + 4;
      th_store32_imm_or_reg_ex(lo_reg, (uint32_t)addr_r, addend < 0 ? (int)(-addend) : (int)addend, addend < 0 ? 1 : 0,
                               excl | (1u << addr_r));
      th_store32_imm_or_reg_ex(hi_reg, (uint32_t)addr_r, addend_hi < 0 ? (int)(-addend_hi) : (int)addend_hi,
                               addend_hi < 0 ? 1 : 0, excl | (1u << addr_r));
      break;
    }

    case MACH_OP_IMM:
    {
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
      th_store32_imm_or_reg_ex(lo_reg, (uint32_t)addr_r, 0, 0, excl | (1u << addr_r));
      th_store32_imm_or_reg_ex(hi_reg, (uint32_t)addr_r, 4, 0, excl | (1u << addr_r));
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
      th_store32_imm_or_reg_ex(lo_reg, (uint32_t)base, abs_off, sign, excl | (1u << (uint32_t)base));
      th_store32_imm_or_reg_ex(hi_reg, (uint32_t)base, abs_off_hi, sign_hi, excl | (1u << (uint32_t)base));
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

  /* Get source value register — may allocate a scratch if spilled/const */
  const int src_reg = mach_ensure_in_reg(&ctx, &src, 0);

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
        ot_check(th_mov_reg((uint32_t)dreg, (uint32_t)src_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE, false));
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
  thumb_shift shift = {.type = THUMB_SHIFT_LSL, .value = (uint32_t)shift_amount, .mode = THUMB_SHIFT_IMMEDIATE};

  /* 64-bit indexed load: compute EA = base + index<<shift into scratch, then LDRD. */
  if (dest.is_64bit)
  {
    const int dest_lo = dest.u.reg.r0;
    if (!thumb_is_hw_reg(dest.u.reg.r1))
      tcc_error("load_indexed_mop: 64-bit dest has invalid r1=%d (r0=%d) — "
                "register allocator must produce a valid pair",
                dest.u.reg.r1, dest.u.reg.r0);
    const int dest_hi = dest.u.reg.r1;
    uint32_t excl = (1u << (uint32_t)dest_lo) | (1u << (uint32_t)dest_hi);
    int base_reg = mach_ensure_in_reg(&ctx, &base, excl);
    excl |= (1u << (uint32_t)base_reg);
    int index_reg = mach_ensure_in_reg(&ctx, &index, excl);
    excl |= (1u << (uint32_t)index_reg);
    int ea_r = mach_alloc_scratch(&ctx, excl);
    ot_check(th_add_reg((uint32_t)ea_r, (uint32_t)base_reg, (uint32_t)index_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, shift,
                        ENFORCE_ENCODING_NONE));
    ot_check(th_ldrd_imm((uint32_t)dest_lo, (uint32_t)dest_hi, (uint32_t)ea_r, 0, 5, ENFORCE_ENCODING_NONE));
    mach_release_all(&ctx);
    return;
  }

  const int dest_reg = dest.u.reg.r0;
  const int btype = dest.btype;
  const int is_unsigned = (int)dest.is_unsigned;

  uint32_t excl = (1u << (uint32_t)dest_reg);
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
  thumb_shift shift = {.type = THUMB_SHIFT_LSL, .value = (uint32_t)shift_amount, .mode = THUMB_SHIFT_IMMEDIATE};

  /* 64-bit indexed store: compute EA = base + index<<shift into scratch, then STRD. */
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
    int base_reg = mach_ensure_in_reg(&ctx, &base, excl);
    excl |= (1u << (uint32_t)base_reg);
    int index_reg = mach_ensure_in_reg(&ctx, &index, excl);
    excl |= (1u << (uint32_t)index_reg);
    int ea_r = mach_alloc_scratch(&ctx, excl);
    ot_check(th_add_reg((uint32_t)ea_r, (uint32_t)base_reg, (uint32_t)index_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, shift,
                        ENFORCE_ENCODING_NONE));
    ot_check(th_strd_imm((uint32_t)lo_reg, (uint32_t)hi_reg, (uint32_t)ea_r, 0, 5, ENFORCE_ENCODING_NONE));
    mach_release_all(&ctx);
    return;
  }

  const int btype = value.btype;

  int value_reg = mach_ensure_in_reg(&ctx, &value, 0);
  uint32_t excl = (1u << (uint32_t)value_reg);
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
    const int dest_lo = dest.u.reg.r0;
    if (!thumb_is_hw_reg(dest.u.reg.r1))
      tcc_error("load_postinc_mop: 64-bit dest has invalid r1=%d (r0=%d) — "
                "register allocator must produce a valid pair",
                dest.u.reg.r1, dest.u.reg.r0);
    const int dest_hi = dest.u.reg.r1;
    uint32_t excl = (1u << (uint32_t)dest_lo) | (1u << (uint32_t)dest_hi);
    int ptr_reg = mach_ensure_in_reg(&ctx, &ptr, excl);
    ot_check(
        th_ldrd_imm((uint32_t)dest_lo, (uint32_t)dest_hi, (uint32_t)ptr_reg, offset_imm, puw, ENFORCE_ENCODING_NONE));
    mach_release_all(&ctx);
    return;
  }

  const int dest_reg = dest.u.reg.r0;
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
    ot_check(th_ldr_imm(dest_reg, ptr_reg, offset_imm, puw, ENFORCE_ENCODING_NONE));
  }
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
        th_strd_imm((uint32_t)lo_reg, (uint32_t)hi_reg, (uint32_t)ptr_reg, offset_imm, puw, ENFORCE_ENCODING_NONE));
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
    ot_check(th_str_imm(value_reg, ptr_reg, offset_imm, puw, ENFORCE_ENCODING_NONE));

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
        ot_check(th_mov_reg((uint32_t)target_reg, (uint32_t)op->u.reg.r0, FLAGS_BEHAVIOUR_NOT_IMPORTANT,
                            THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
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
  ot_check(th_str_imm(R0, R_SP, off, 6, ENFORCE_ENCODING_NONE));
  ot_check(th_str_imm(R1, R_SP, off + 4, 6, ENFORCE_ENCODING_NONE));
}

/* Helper: load a double from SP-relative stack offset into (lo_reg, hi_reg). */
static void fp_mop_load_double_from_sp(int lo_reg, int hi_reg, int off)
{
  ot_check(th_ldr_imm(lo_reg, R_SP, off, 6, ENFORCE_ENCODING_NONE));
  ot_check(th_ldr_imm(hi_reg, R_SP, off + 4, 6, ENFORCE_ENCODING_NONE));
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
    ot_check(th_sub_sp_imm(R_SP, 8, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

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

    ot_check(th_add_sp_imm(R_SP, 8, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }
  else if (s1_complex && !s2_complex)
  {
    /* complex double × scalar double: (a+bi) * c = ac + (bc)i */
    MachineOperand s1_real = mach_make_complex_real(&src1);
    MachineOperand s1_imag = mach_make_complex_imag(&src1);

    /* Allocate 8 bytes to save the scalar 'c'. */
    ot_check(th_sub_sp_imm(R_SP, 8, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

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

    ot_check(th_add_sp_imm(R_SP, 8, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
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

    ot_check(th_sub_sp_imm(R_SP, 48, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

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

    ot_check(th_add_sp_imm(R_SP, 48, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
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
    ot_check(th_mov_reg((uint32_t)tmp, (uint32_t)hi_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                        ENFORCE_ENCODING_NONE, false));
    mach_writeback_dest(d_lo, lo_reg);
    mach_writeback_dest(d_hi, tmp);
  }
  else if (lo_clobbers_hi)
  {
    /* Lo writeback would clobber hi value; save hi first */
    int tmp = (lo_reg != R2 && hi_reg != R2) ? R2 : R3;
    ot_check(th_mov_reg((uint32_t)tmp, (uint32_t)hi_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                        ENFORCE_ENCODING_NONE, false));
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
  ot_check(th_sub_sp_imm(R_SP, 32, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

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

  ot_check(th_add_sp_imm(R_SP, 32, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
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
  ot_check(th_sub_sp_imm(R_SP, 16, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

  /* Load and save each component to stack. */
  fp_mop_load_arg(R0, &s1_real);
  ot_check(th_str_imm(R0, R_SP, 0, 6, ENFORCE_ENCODING_NONE));
  fp_mop_load_arg(R0, &s1_imag);
  ot_check(th_str_imm(R0, R_SP, 4, 6, ENFORCE_ENCODING_NONE));
  fp_mop_load_arg(R0, &s2_real);
  ot_check(th_str_imm(R0, R_SP, 8, 6, ENFORCE_ENCODING_NONE));
  fp_mop_load_arg(R0, &s2_imag);
  ot_check(th_str_imm(R0, R_SP, 12, 6, ENFORCE_ENCODING_NONE));

  /* Compute real part: func(a.real, b.real) */
  ot_check(th_ldr_imm(R0, R_SP, 0, 6, ENFORCE_ENCODING_NONE));
  ot_check(th_ldr_imm(R1, R_SP, 8, 6, ENFORCE_ENCODING_NONE));
  fp_mop_do_bl(func_name);
  /* Save real result to stack slot 0 */
  ot_check(th_str_imm(R0, R_SP, 0, 6, ENFORCE_ENCODING_NONE));

  /* Compute imag part: func(a.imag, b.imag) */
  ot_check(th_ldr_imm(R0, R_SP, 4, 6, ENFORCE_ENCODING_NONE));
  ot_check(th_ldr_imm(R1, R_SP, 12, 6, ENFORCE_ENCODING_NONE));
  fp_mop_do_bl(func_name);
  /* R0 = imag result */

  /* Load real result from stack, deallocate, write back. */
  MachineOperand d_real = mach_make_lo_half(&dest);
  MachineOperand d_imag = mach_make_hi_half(&dest);

  /* R0 = imag result.  Load real result from stack into R1. */
  ot_check(th_ldr_imm(R1, R_SP, 0, 6, ENFORCE_ENCODING_NONE));
  ot_check(th_add_sp_imm(R_SP, 16, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

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
  ot_check(th_sub_sp_imm(R_SP, 24, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

  /* Save inputs to stack */
  fp_mop_load_arg(R0, &s1_real);
  ot_check(th_str_imm(R0, R_SP, 8, 6, ENFORCE_ENCODING_NONE)); /* a */
  fp_mop_load_arg(R0, &s1_imag);
  ot_check(th_str_imm(R0, R_SP, 12, 6, ENFORCE_ENCODING_NONE)); /* b */
  fp_mop_load_arg(R0, &s2_real);
  ot_check(th_str_imm(R0, R_SP, 16, 6, ENFORCE_ENCODING_NONE)); /* c */
  fp_mop_load_arg(R0, &s2_imag);
  ot_check(th_str_imm(R0, R_SP, 20, 6, ENFORCE_ENCODING_NONE)); /* d */

  const int off_scratch0 = 0;
  const int off_scratch1 = 4;
  const int off_a = 8;
  const int off_b = 12;
  const int off_c = 16;
  const int off_d = 20;

  /* Step 1: ac = a * c → scratch0 */
  ot_check(th_ldr_imm(R0, R_SP, off_a, 6, ENFORCE_ENCODING_NONE));
  ot_check(th_ldr_imm(R1, R_SP, off_c, 6, ENFORCE_ENCODING_NONE));
  fp_mop_do_bl("__aeabi_fmul");
  ot_check(th_str_imm(R0, R_SP, off_scratch0, 6, ENFORCE_ENCODING_NONE));

  /* Step 2: bd = b * d → scratch1 */
  ot_check(th_ldr_imm(R0, R_SP, off_b, 6, ENFORCE_ENCODING_NONE));
  ot_check(th_ldr_imm(R1, R_SP, off_d, 6, ENFORCE_ENCODING_NONE));
  fp_mop_do_bl("__aeabi_fmul");
  ot_check(th_str_imm(R0, R_SP, off_scratch1, 6, ENFORCE_ENCODING_NONE));

  /* Step 3: real = ac - bd → scratch0 */
  ot_check(th_ldr_imm(R0, R_SP, off_scratch0, 6, ENFORCE_ENCODING_NONE));
  ot_check(th_ldr_imm(R1, R_SP, off_scratch1, 6, ENFORCE_ENCODING_NONE));
  fp_mop_do_bl("__aeabi_fsub");
  ot_check(th_str_imm(R0, R_SP, off_scratch0, 6, ENFORCE_ENCODING_NONE));

  /* Step 4: ad = a * d → scratch1 */
  ot_check(th_ldr_imm(R0, R_SP, off_a, 6, ENFORCE_ENCODING_NONE));
  ot_check(th_ldr_imm(R1, R_SP, off_d, 6, ENFORCE_ENCODING_NONE));
  fp_mop_do_bl("__aeabi_fmul");
  ot_check(th_str_imm(R0, R_SP, off_scratch1, 6, ENFORCE_ENCODING_NONE));

  /* Step 5: bc = b * c → off_a (no longer needed) */
  ot_check(th_ldr_imm(R0, R_SP, off_b, 6, ENFORCE_ENCODING_NONE));
  ot_check(th_ldr_imm(R1, R_SP, off_c, 6, ENFORCE_ENCODING_NONE));
  fp_mop_do_bl("__aeabi_fmul");
  ot_check(th_str_imm(R0, R_SP, off_a, 6, ENFORCE_ENCODING_NONE));

  /* Step 6: imag = ad + bc → scratch1 */
  ot_check(th_ldr_imm(R0, R_SP, off_scratch1, 6, ENFORCE_ENCODING_NONE));
  ot_check(th_ldr_imm(R1, R_SP, off_a, 6, ENFORCE_ENCODING_NONE));
  fp_mop_do_bl("__aeabi_fadd");
  ot_check(th_str_imm(R0, R_SP, off_scratch1, 6, ENFORCE_ENCODING_NONE));

  /* Load results and write back */
  MachineOperand d_real = mach_make_lo_half(&dest);
  MachineOperand d_imag = mach_make_hi_half(&dest);
  ot_check(th_ldr_imm(R0, R_SP, off_scratch0, 6, ENFORCE_ENCODING_NONE)); /* real */
  ot_check(th_ldr_imm(R1, R_SP, off_scratch1, 6, ENFORCE_ENCODING_NONE)); /* imag */
  ot_check(th_add_sp_imm(R_SP, 24, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

  complex_pair_writeback(&d_real, R0, &d_imag, R1);
}

/* Process complex float division via MachineOperands.
 * Calls __divsc3 from libgcc for numerically robust division.
 *
 * __divsc3 calling convention (soft-float AAPCS, hidden return pointer):
 *   R0       = hidden return pointer (8-byte buffer for result)
 *   R1       = a_re (float)
 *   R2       = a_im (float)
 *   R3       = b_re (float)
 *   [sp+0]   = b_im (float, on stack)
 *   Result written to [R0+0..3] = real, [R0+4..7] = imag
 *
 * Stack layout (24 bytes, 8-byte aligned):
 *   [sp+0]   = b_im for __divsc3 stack arg  (4 bytes)
 *   [sp+4]   = a_re staging                 (4 bytes)
 *   [sp+8]   = a_im staging                 (4 bytes)
 *   [sp+12]  = b_re staging                 (4 bytes)
 *   [sp+16]  = result buffer: real part      (4 bytes)
 *   [sp+20]  = result buffer: imag part      (4 bytes)
 */
static void thumb_process_complex_div_mop(MachineOperand src1, MachineOperand src2, MachineOperand dest)
{
  MachineOperand s1_real = mach_make_lo_half(&src1);
  MachineOperand s1_imag = mach_make_hi_half(&src1);
  MachineOperand s2_real = mach_make_lo_half(&src2);
  MachineOperand s2_imag = mach_make_hi_half(&src2);

  /* Allocate 24 bytes (8-byte aligned). */
  ot_check(th_sub_sp_imm(R_SP, 24, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

  /* Stage all four operands to stack via R0 to avoid clobbering. */
  fp_mop_load_arg(R0, &s2_imag);
  ot_check(th_str_imm(R0, R_SP, 0, 6, ENFORCE_ENCODING_NONE)); /* b_im → [sp+0] (stack arg) */
  fp_mop_load_arg(R0, &s1_real);
  ot_check(th_str_imm(R0, R_SP, 4, 6, ENFORCE_ENCODING_NONE)); /* a_re → [sp+4] */
  fp_mop_load_arg(R0, &s1_imag);
  ot_check(th_str_imm(R0, R_SP, 8, 6, ENFORCE_ENCODING_NONE)); /* a_im → [sp+8] */
  fp_mop_load_arg(R0, &s2_real);
  ot_check(th_str_imm(R0, R_SP, 12, 6, ENFORCE_ENCODING_NONE)); /* b_re → [sp+12] */

  /* Load register args from staging area. */
  ot_check(th_ldr_imm(R1, R_SP, 4, 6, ENFORCE_ENCODING_NONE));  /* R1 = a_re */
  ot_check(th_ldr_imm(R2, R_SP, 8, 6, ENFORCE_ENCODING_NONE));  /* R2 = a_im */
  ot_check(th_ldr_imm(R3, R_SP, 12, 6, ENFORCE_ENCODING_NONE)); /* R3 = b_re */

  /* R0 = pointer to result buffer at [sp+16]. */
  ot_check(th_add_sp_imm(R0, 16, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

  /* Call __divsc3. */
  fp_mop_do_bl("__divsc3");

  /* Read result from buffer and write back to dest. */
  MachineOperand d_real = mach_make_lo_half(&dest);
  MachineOperand d_imag = mach_make_hi_half(&dest);
  ot_check(th_ldr_imm(R0, R_SP, 16, 6, ENFORCE_ENCODING_NONE)); /* real */
  ot_check(th_ldr_imm(R1, R_SP, 20, 6, ENFORCE_ENCODING_NONE)); /* imag */

  ot_check(th_add_sp_imm(R_SP, 24, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

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

  /* Allocate 40 bytes (8-byte aligned). */
  ot_check(th_sub_sp_imm(R_SP, 40, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

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
  ot_check(th_add_sp_imm(R0, 24, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));

  /* Call __divdc3. */
  fp_mop_do_bl("__divdc3");

  /* Read result from buffer and write back to dest. */
  fp_mop_load_double_from_sp(R0, R1, 24);
  fp_mop_writeback_result(&d_real, 1);
  fp_mop_load_double_from_sp(R0, R1, 32);
  fp_mop_writeback_result(&d_imag, 1);

  ot_check(th_add_sp_imm(R_SP, 40, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
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
      load_full_const(scr.reg, PREG_NONE, 0x80000000, NULL);
      ot_check(th_eor_reg(R1, R1, scr.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      restore_scratch_reg(&scr);
      fp_mop_writeback_result(&dest, 1);
    }
    else
    {
      /* f32: R0 ^= 0x80000000 */
      fp_mop_load_arg(R0, &src1);
      scr = get_scratch_reg_with_save(1u << R0);
      load_full_const(scr.reg, PREG_NONE, 0x80000000, NULL);
      ot_check(th_eor_reg(R0, R0, scr.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
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
    ot_check(th_mov_reg(R0, src_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
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

  const int need_fp = (tcc_state->force_frame_pointer || tcc_state->need_frame_pointer || (stack_size > 0));
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

    /* Pad total to even count for 8-byte alignment (AAPCS). */
    int total = frame_count + callee_count;
    if (total % 2 != 0)
    {
      callee_regs_local |= (1 << R12);
      callee_count++;
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
    if (!ot(th_add_imm(R_FP, R_SP, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)))
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

    /* Keep the total push size 8-byte aligned (AAPCS). */
    if (registers_count % 2 != 0)
    {
      registers_to_push |= (1 << R12);
      registers_count++;
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
  allocated_stack_size = stack_size;
  if (tcc_state->need_frame_pointer && !standard_frame_record)
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
    ot_check(th_add_imm(R12, R_FP, offset_to_args, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
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
    ot_check(th_add_imm(R_IP, R_FP, offset_to_args, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
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

  if (tcc_state->need_frame_pointer && callee_saved_regs)
  {
    /* ── Two-phase pop (mirrors two-phase push) ── */
    /* Restore SP from FP (works even with alloca/VLA since FP is stable) */
    ot_check(th_mov_reg(R_SP, R_FP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
    /* SP = FP; callee-saved regs are below FP. Adjust SP down. */
    gadd_sp(-callee_push_size);
    ot_check(th_pop(callee_saved_regs));
    /* SP is now at FP (pointing at frame record {r7, [lr]}) */
    if (vararg_push_size > 0 && lr_saved)
    {
      /* Variadic: pop FP+LR, then skip over the pushed r0-r3 area */
      ot_check(th_pop((1 << R_FP) | (1 << R_LR)));
      gadd_sp(vararg_push_size);
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
        gadd_sp(vararg_push_size);
      ot_check(th_bx_reg(R_LR));
    }
  }
  else if (tcc_state->need_frame_pointer)
  {
    /* ── Original single-push with FP: restore SP from FP, then pop all ── */
    ot_check(th_mov_reg(R_SP, R_FP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
    if (vararg_push_size > 0 && lr_saved)
    {
      /* Variadic: pop all regs with LR (not PC), then skip pushed r0-r3 */
      ot_check(th_pop(pushed_registers));
      gadd_sp(vararg_push_size);
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
        gadd_sp(vararg_push_size);
      ot_check(th_bx_reg(R_LR));
    }
  }
  else
  {
    /* ── No frame pointer ── */
    if (allocated_stack_size > 0)
      gadd_sp(allocated_stack_size);
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

  switch (src.kind)
  {
  case MACH_OP_PARAM_STACK:
  {
    /* Compute address of caller's argument slot. */
    r = mach_alloc_scratch(&ctx, 0);
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
    r = mach_alloc_scratch(&ctx, excl | (1u << (uint32_t)base));
    int32_t off = src.u.chain.offset;
    int sign = (off < 0);
    int abs_off = sign ? (int)(-off) : (int)off;
    if (abs_off == 0)
    {
      if (r != base)
        ot_check(th_mov_reg((uint32_t)r, (uint32_t)base, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE, false));
    }
    else
    {
      thumb_opcode ins = sign ? th_sub_imm(r, base, abs_off, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)
                              : th_add_imm(r, base, abs_off, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
      if (ins.size != 0)
      {
        ot_check(ins);
      }
      else
      {
        /* Large offset: load into a scratch and use register ADD/SUB */
        ScratchRegAlloc off_sc = get_scratch_reg_with_save(excl | (1u << (uint32_t)r) | (1u << (uint32_t)base));
        load_full_const(off_sc.reg, PREG_NONE, abs_off, NULL);
        ot_check(sign ? th_sub_reg(r, base, off_sc.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                   ENFORCE_ENCODING_NONE)
                      : th_add_reg(r, base, off_sc.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                   ENFORCE_ENCODING_NONE));
        restore_scratch_reg(&off_sc);
      }
    }
    if (chain_used)
      restore_scratch_reg(&chain_scratch);
    break;
  }
  default:
    /* FRAME_ADDR, SYMBOL, REG: mach_ensure_in_reg already computes the address. */
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
  int sign = (offset < 0);
  int abs_offset = sign ? -offset : offset;

  /* Try direct STR with immediate offset */
  if (!store_word_to_base(reg, R_FP, abs_offset, sign))
  {
    /* Offset too large, use scratch register */
    /* Don't reuse the source register as offset scratch, otherwise we'd
     * clobber the value before the STR (e.g. store -offset instead of value). */
    ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(abs_offset, sign, (1u << reg) | (1u << R_FP) | extra_exclude);
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
  if (target.kind == MACH_OP_SYMBOL)
  {
    /* Direct call via BL with relocation. */
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
      ot_check(th_bl_t1(imm));
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
    if (r != R_IP)
    {
      thumb_shift no_shift = {THUMB_SHIFT_NONE, 0, THUMB_SHIFT_IMMEDIATE};
      ot_check(th_mov_reg(R_IP, r, FLAGS_BEHAVIOUR_NOT_IMPORTANT, no_shift, ENFORCE_ENCODING_NONE, false));
    }
    ot_check(th_bx_reg(R_IP));
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

  if (m->kind == THUMB_ARG_MOVE_STRUCT)
  {
    /* Load struct words into consecutive registers.
     * The mop contains the struct operand; get its base address. */
    int word_count = m->struct_word_count;
    int base_dst = m->dst_reg;

    /* Get the struct base address into a scratch register */
    int base_addr_reg = get_struct_base_addr_mop(&m->mop, ARM_R12);

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
        load_from_base(m->dst_reg, PREG_REG_NONE, IROP_BTYPE_INT32, 0, 0, 0, (uint32_t)base);
        load_from_base(m->dst_reg_hi, PREG_REG_NONE, IROP_BTYPE_INT32, 0, 4, 0, (uint32_t)base);
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
          ot_check(th_mov_reg(m->dst_reg, r_lo, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE, false));
        if (r_hi != m->dst_reg_hi)
          ot_check(th_mov_reg(m->dst_reg_hi, r_hi, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                              ENFORCE_ENCODING_NONE, false));
      }
    }
    else
    {
      /* 32-bit: single-register load. */
      uint32_t excl = (1u << m->dst_reg);
      int r = mach_ensure_in_reg(&mctx, &m->mop, excl);
      if (r != m->dst_reg)
        ot_check(th_mov_reg(m->dst_reg, r, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE,
                            false));
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
                                 const TCCAbiArgLoc *loc, int base_reg, ThumbGenCallSite *call_site)
{
  int words = loc->reg_count;
  if (words > 0 && words <= 4)
  {
    moves[move_count++] = (ThumbArgMove){
        .kind = THUMB_ARG_MOVE_STRUCT,
        .dst_reg = base_reg,
        .mop = *mop,
        .struct_word_count = words,
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
static void place_stack_arg_struct(const MachineOperand *mop, const TCCAbiArgLoc *loc, int stack_offset)
{
  int words_in_regs = (loc->kind == TCC_ABI_LOC_REG_STACK) ? loc->reg_count : 0;
  int struct_src_offset = words_in_regs * 4;
  int struct_size = (loc->kind == TCC_ABI_LOC_REG_STACK) ? loc->stack_size : loc->size;
  int words = (struct_size + 3) / 4;

  int base_addr_reg = get_struct_base_addr_mop(mop, ARM_R12);

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

/* Place a 64-bit argument on stack (MOP path) */
static void place_stack_arg_64bit(const MachineOperand *mop, int stack_offset, TCCIRState *ir)
{
  int lo_offset = stack_offset;
  int hi_offset = stack_offset + 4;

  if (mop->kind == MACH_OP_REG && !mop->needs_deref && thumb_is_hw_reg(mop->u.reg.r0) && thumb_is_hw_reg(mop->u.reg.r1))
  {
    store_word_to_stack(mop->u.reg.r0, lo_offset);
    store_word_to_stack(mop->u.reg.r1, hi_offset);
  }
  else if (mop->kind == MACH_OP_IMM)
  {
    uint64_t imm64 = (uint64_t)mop->u.imm.val;
    load_immediate(ARM_R12, (uint32_t)imm64, NULL, false);
    store_word_to_stack(ARM_R12, lo_offset);
    load_immediate(ARM_R12, (uint32_t)(imm64 >> 32), NULL, false);
    store_word_to_stack(ARM_R12, hi_offset);
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
     * The base register must NOT be ARM_R12 because both halves are loaded
     * into ARM_R12 (the scratch destination).  If base == ARM_R12 the first
     * load would clobber the pointer before the second load can use it. */
    int base;
    MachineCodegenContext mctx = {0};
    bool need_release = false;
    if (mop->kind == MACH_OP_REG && mop->u.reg.r0 != ARM_R12)
    {
      base = mop->u.reg.r0;
    }
    else
    {
      MachineOperand addr = *mop;
      addr.needs_deref = false;
      addr.is_64bit = false;
      addr.btype = IROP_BTYPE_INT32;
      base = mach_ensure_in_reg(&mctx, &addr, (1u << ARM_R12));
      need_release = true;
    }
    load_from_base(ARM_R12, PREG_REG_NONE, IROP_BTYPE_INT32, 0, 0, 0, (uint32_t)base);
    store_word_to_stack(ARM_R12, lo_offset);
    load_from_base(ARM_R12, PREG_REG_NONE, IROP_BTYPE_INT32, 0, 4, 0, (uint32_t)base);
    store_word_to_stack(ARM_R12, hi_offset);
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
      /* Register-indirect: load through the register, then store to stack. */
      ot_check(th_ldr_imm(ARM_R12, mop->u.reg.r0, 0, 6, ENFORCE_ENCODING_NONE));
      store_word_to_stack(ARM_R12, stack_offset);
    }
    break;

  case MACH_OP_IMM:
    load_immediate(ARM_R12, (uint32_t)mop->u.imm.val, NULL, false);
    store_word_to_stack(ARM_R12, stack_offset);
    break;

  case MACH_OP_SYMBOL:
  {
    Sym *sym = mop->u.sym.sym ? validate_sym_for_reloc(mop->u.sym.sym) : NULL;
    if (mop->needs_deref)
    {
      /* Load value from global symbol address. */
      load_immediate(ARM_R12, 0, sym, false);
      int32_t addend = mop->u.sym.addend;
      int sign = (addend < 0);
      int abs_off = sign ? -addend : addend;
      load_from_base(ARM_R12, PREG_REG_NONE, mop->btype, mop->is_unsigned, abs_off, sign, ARM_R12);
    }
    else
    {
      load_immediate(ARM_R12, (uint32_t)mop->u.sym.addend, sym, false);
    }
    store_word_to_stack(ARM_R12, stack_offset);
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
        move_count = build_reg_move_struct(reg_moves, move_count, mop, loc, base_reg, ctx->call_site);
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
    if (bt == IROP_BTYPE_STRUCT || mop->is_64bit || mop->is_complex)
      continue;

    /* Only pre-save if operand is in R0-R3 (arg registers that get overwritten). */
    if (mop->kind == MACH_OP_REG && !mop->needs_deref && mop->u.reg.r0 <= ARM_R3)
    {
      store_word_to_stack(mop->u.reg.r0, loc->stack_off);
    }
  }
}

/* Place all stack arguments */
static void place_stack_arguments(CallGenContext *ctx)
{
  for (int i = 0; i < ctx->argc; ++i)
  {
    const TCCAbiArgLoc *loc = &ctx->layout->locs[i];
    const MachineOperand *mop = &ctx->mops[i];

    if (loc->kind == TCC_ABI_LOC_REG)
      continue;

    int stack_offset = loc->stack_off;

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
        for (int w = 0; w < stack_words; w++)
        {
          int word_idx = words_in_regs + w;
          uint32_t word_val = (uint32_t)(imm64 >> (word_idx * 32));
          load_immediate(ARM_R12, word_val, NULL, false);
          store_word_to_stack(ARM_R12, stack_offset + w * 4);
        }
      }
      else
      {
        place_stack_arg_struct(mop, loc, stack_offset);
      }
    }
    else if (mop->is_64bit)
      place_stack_arg_64bit(mop, stack_offset, tcc_state->ir);
    else
      place_stack_arg_32bit(mop, stack_offset, ctx);
  }
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

  /* === Preserve nested call registers (R0-R3) === */
  int arg_regs_in_use = call_site->registers_map & 0x0F;
  int arg_regs_push_mask = arg_regs_in_use;
  int arg_regs_push_count = __builtin_popcount((unsigned)arg_regs_push_mask);

  /* On yasos with no-pic-data-is-text-relative, R9 holds the GOT base and is
   * caller-saved.  Save it alongside the nested-call argument registers so it
   * is restored after the callee returns.  It must be pushed *before* the
   * stack-argument area is reserved so the callee sees the correct SP layout.
   */
  if (text_and_data_separation)
  {
    arg_regs_push_mask |= (1 << ARM_R9);
    arg_regs_push_count++;
  }

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
      uint32_t exclude = scratch_global_exclude | (1u << R_IP) | (1u << R_SP) | (1u << R_PC);
      int safe_reg = PREG_NONE;
      if (ir)
        safe_reg = tcc_ls_find_free_scratch_reg(&ir->ls, ir->codegen_instruction_idx, exclude, ir->leaffunc);

      if (safe_reg == PREG_NONE || safe_reg < 0 || safe_reg >= 16 || safe_reg == R_SP || safe_reg == R_PC)
        tcc_error("compiler_error: func_call_mop: cannot find safe register "
                  "to pre-save indirect call target (R%d)",
                  func_mop.u.reg.r0);

      /* Move function pointer from arg reg to safe reg. */
      thumb_shift no_shift = {THUMB_SHIFT_NONE, 0, THUMB_SHIFT_IMMEDIATE};
      ot_check(th_mov_reg(safe_reg, func_mop.u.reg.r0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, no_shift, ENFORCE_ENCODING_NONE,
                          false));

      /* Rewrite func_mop to point to the safe register. */
      func_mop.kind = MACH_OP_REG;
      func_mop.u.reg.r0 = safe_reg;
      func_mop.u.reg.r1 = -1;
      func_mop.needs_deref = false;

      /* Protect the safe register from scratch allocation during arg setup. */
      scratch_global_exclude |= (1u << safe_reg);
    }
  }

  /* === Build and execute register argument moves === */
  ThumbArgMove reg_moves[8];
  int reg_move_count = build_register_arg_moves(&ctx, reg_moves);

  /* Pre-save stack args sourcing from R0-R3 before register shuffle */
  presave_stack_args_from_arg_regs(&ctx);

  thumb_emit_parallel_arg_moves(reg_moves, reg_move_count);

  /* === Place stack arguments === */
  place_stack_arguments(&ctx);

  /* === Emit call === */
  gcall_or_jump_mop(0, func_mop);
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

  handle_return_value_mop(&dest_mop, drop_value);

  call_site->registers_map &= ~0x0F; /* Clear R0-R3 */

  if (args)
    tcc_free(args);
  if (mops)
    tcc_free(mops);
  if (layout.locs)
    tcc_free(layout.locs);
}

ST_FUNC void tcc_gen_machine_jump_mop(TccIrOp op, int32_t target_ir, int ir_idx)
{

  if (dry_run_state.active)
  {
    /* Record branch for later optimization analysis */
    branch_opt_record(ir_idx, ind, target_ir, 0); /* 0 = unconditional */
    /* Emit 32-bit placeholder for code size tracking */
    ot_check(th_b_t4(0));
    return;
  }

  /* Real pass: check if we determined this can be 16-bit */
  BranchEncoding enc = branch_opt_get_encoding(ir_idx);
  if (enc == BRANCH_ENC_16BIT)
  {
    ot_check(th_b_t2(0)); /* 16-bit placeholder */
  }
  else
  {
    ot_check(th_b_t4(0)); /* 32-bit placeholder */
  }
}

ST_FUNC void tcc_gen_machine_conditional_jump_mop(int32_t condition, TccIrOp op, int32_t target_ir, int ir_idx)
{
  int cond = mapcc(condition);

  if (dry_run_state.active)
  {
    /* Record branch for later optimization analysis */
    branch_opt_record(ir_idx, ind, target_ir, 1); /* 1 = conditional */
    /* Emit 32-bit placeholder for code size tracking */
    ot_check(th_b_t3(cond, 0));
    return;
  }

  /* Real pass: check if we determined this can be 16-bit */
  BranchEncoding enc = branch_opt_get_encoding(ir_idx);
  if (enc == BRANCH_ENC_16BIT)
  {
    ot_check(th_b_t1(cond, 0)); /* 16-bit conditional */
  }
  else
  {
    ot_check(th_b_t3(cond, 0)); /* 32-bit conditional */
  }
}

/* Set static chain register: MOV R10, R7 (FP) */
ST_FUNC void tcc_gen_machine_set_chain(void)
{
  int chain_reg = architecture_config.static_chain_reg;
  thumb_shift no_shift = {THUMB_SHIFT_NONE, 0, THUMB_SHIFT_IMMEDIATE};
  /* MOV chain_reg, R_FP (R7 on ARM Thumb) */
  ot_check(th_mov_reg(chain_reg, R_FP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, no_shift, ENFORCE_ENCODING_NONE, false));
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

  /* Load chain slot address into scratch register via literal pool */
  load_full_const(scratch.reg, PREG_NONE, 0, chain_sym);

  /* STR R7, [scratch, #0] — store frame pointer into chain slot */
  ot_check(th_str_imm(R_FP, scratch.reg, 0, 6, ENFORCE_ENCODING_NONE));

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
    ot_check(th_sub_sp_reg(r, r, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));

    if (align > 1)
    {
      /* Align down: r &= ~(align-1).  Try immediate BIC first. */
      if (!ot(th_bic_imm(r, r, (uint32_t)(align - 1), FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)))
      {
        /* Fallback: materialize mask in a scratch register. */
        int mask_reg = mach_alloc_scratch(&ctx, 1u << (uint32_t)r);
        if (!ot(th_generic_mov_imm(mask_reg, align - 1)))
          load_full_const(mask_reg, PREG_NONE, align - 1, NULL);
        ot_check(th_bic_reg(r, r, mask_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
      }
    }

    ot_check(th_mov_reg(R_SP, r, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
    break;
  }
  case TCCIR_OP_VLA_SP_SAVE:
    /* Save current SP to the destination save slot via IP as intermediary. */
    ot_check(th_mov_reg(R_IP, R_SP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
    mach_writeback_dest(&dest, R_IP);
    break;
  case TCCIR_OP_VLA_SP_RESTORE:
  {
    /* Load the saved SP from src1 into a register, then restore SP. */
    int saved_sp = mach_ensure_in_reg(&ctx, &src1, 0);
    ot_check(
        th_mov_reg(R_SP, saved_sp, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
    break;
  }
  default:
    tcc_error("compiler_error: tcc_gen_machine_vla_mop unsupported op %d", op);
  }
  mach_release_all(&ctx);
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
    /* Load offset into IP (R12), add FP, then PLD [R12] */
    int32_t offset = addr.u.spill.offset;
    if (offset != 0)
    {
      load_full_const(ARM_R12, PREG_NONE, offset, NULL);
      ot_check(th_add_reg(ARM_R12, R_FP, ARM_R12, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      ot_check(th_pld_imm(ARM_R12, 0, 0));
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
    /* Use R12 (IP) as scratch since it's caller-saved */
    load_full_const(ARM_R12, PREG_NONE, addr.u.imm.val, NULL);
    ot_check(th_pld_imm(ARM_R12, 0, 0));
    break;
  }
  case MACH_OP_SYMBOL:
  {
    /* For symbol addresses, load into a register first */
    load_full_const(ARM_R12, PREG_NONE, addr.u.sym.addend, addr.u.sym.sym);
    ot_check(th_pld_imm(ARM_R12, 0, 0));
    break;
  }
  case MACH_OP_FRAME_ADDR:
  {
    /* Frame address: FP + offset */
    int32_t offset = addr.u.frame.offset;
    if (offset != 0)
    {
      load_full_const(ARM_R12, PREG_NONE, offset, NULL);
      ot_check(th_add_reg(ARM_R12, R_FP, ARM_R12, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                          ENFORCE_ENCODING_NONE));
      ot_check(th_pld_imm(ARM_R12, 0, 0));
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
    ot_check(th_mov_imm(buf_reg, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }
  else
  {
    buf_reg = mach_ensure_in_reg(&ctx, &buf, 0);
  }

  /* ---- save frame pointer ---- */
  ot_check(th_str_imm(R_FP, buf_reg, 0, 6, ENFORCE_ENCODING_NONE)); /* r7  -> buf[0]  */

  /* ---- save SP ---- */
  ot_check(th_mov_reg(R_IP, R_SP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
  ot_check(th_str_imm(R_IP, buf_reg, 8, 6, ENFORCE_ENCODING_NONE)); /* SP -> buf[2] */

  /* ---- save resume address (ADR IP, resume_label) ---- */
  int adr_addr = ind;
  int adr_pc = adr_addr + 4;
  int adr_base = adr_pc & ~3;
  int resume_label_addr = adr_addr + 20; /* 4(ORR)+4(STR)+4(MOV)+4(B) after ADR */
  int adr_imm = resume_label_addr - adr_base;
  ot_check(th_adr_imm(R_IP, adr_imm, ENFORCE_ENCODING_32BIT));

  ot_check(th_orr_imm(R_IP, R_IP, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)); /* Thumb bit */
  ot_check(th_str_imm(R_IP, buf_reg, 4, 6, ENFORCE_ENCODING_NONE));                          /* -> buf[1] */

  /* ---- normal path: return 0 ---- */
  int dest_reg = mach_get_dest_reg(&ctx, &dest, 0);
  ot_check(th_mov_imm(dest_reg, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_32BIT)); /* dest = 0 */
  ot_check(th_b_t4(4));                                                                     /* B.W +4 (skip resume) */

  /* ---- resume_label: longjmp lands here ---- */
  ot_check(th_mov_imm(dest_reg, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_32BIT)); /* dest = 1 */
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
    ot_check(th_mov_imm(buf_reg, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE));
  }
  else
  {
    buf_reg = mach_ensure_in_reg(&ctx, &buf, 0);
  }

  /* ---- save callee-saved registers r4-r11 ---- */
  ot_check(th_str_imm(4, buf_reg, 0, 6, ENFORCE_ENCODING_NONE));     /* r4  -> buf[0]  */
  ot_check(th_str_imm(5, buf_reg, 4, 6, ENFORCE_ENCODING_NONE));     /* r5  -> buf[1]  */
  ot_check(th_str_imm(6, buf_reg, 8, 6, ENFORCE_ENCODING_NONE));     /* r6  -> buf[2]  */
  ot_check(th_str_imm(R_FP, buf_reg, 12, 6, ENFORCE_ENCODING_NONE)); /* r7  -> buf[3]  */
  ot_check(th_str_imm(8, buf_reg, 16, 6, ENFORCE_ENCODING_NONE));    /* r8  -> buf[4]  */
  ot_check(th_str_imm(9, buf_reg, 20, 6, ENFORCE_ENCODING_NONE));    /* r9  -> buf[5]  */
  ot_check(th_str_imm(10, buf_reg, 24, 6, ENFORCE_ENCODING_NONE));   /* r10 -> buf[6]  */
  ot_check(th_str_imm(11, buf_reg, 28, 6, ENFORCE_ENCODING_NONE));   /* r11 -> buf[7]  */

  /* ---- save SP ---- */
  ot_check(th_mov_reg(R_IP, R_SP, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
  ot_check(th_str_imm(R_IP, buf_reg, 32, 6, ENFORCE_ENCODING_NONE)); /* SP -> buf[8] */

  /* ---- save resume address (ADR IP, resume_label) ---- */
  int adr_addr = ind;
  int adr_pc = adr_addr + 4;
  int adr_base = adr_pc & ~3;
  int resume_label_addr = adr_addr + 20; /* 4(ORR)+4(STR)+4(MOV)+4(B) after ADR */
  int adr_imm = resume_label_addr - adr_base;
  ot_check(th_adr_imm(R_IP, adr_imm, ENFORCE_ENCODING_32BIT));

  ot_check(th_orr_imm(R_IP, R_IP, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)); /* Thumb bit */
  ot_check(th_str_imm(R_IP, buf_reg, 36, 6, ENFORCE_ENCODING_NONE));                         /* -> buf[9] */

  /* ---- normal path: return 0 ---- */
  int dest_reg = mach_get_dest_reg(&ctx, &dest, 0);
  ot_check(th_mov_imm(dest_reg, 0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_32BIT)); /* dest = 0 */
  ot_check(th_b_t4(4));                                                                     /* B.W +4 (skip resume) */

  /* ---- resume_label: longjmp lands here ---- */
  ot_check(th_mov_imm(dest_reg, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_32BIT)); /* dest = 1 */
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
  ot_check(th_mov_reg(R_IP, buf_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));

  /* Read resume address and saved SP into caller-saved regs first */
  ot_check(th_ldr_imm(0, R_IP, 4, 6, ENFORCE_ENCODING_NONE)); /* r0 = resume addr */
  ot_check(th_ldr_imm(1, R_IP, 8, 6, ENFORCE_ENCODING_NONE)); /* r1 = saved SP    */

  /* Restore frame pointer */
  ot_check(th_ldr_imm(R_FP, R_IP, 0, 6, ENFORCE_ENCODING_NONE)); /* r7 = FP */

  /* Restore SP */
  ot_check(th_mov_reg(R_SP, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));

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
        ot_check(th_mov_reg((uint32_t)buf_reg, (uint32_t)base, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                            ENFORCE_ENCODING_NONE, false));
    }
    else
    {
      thumb_opcode ins = sign
                             ? th_sub_imm(buf_reg, base, abs_off, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE)
                             : th_add_imm(buf_reg, base, abs_off, FLAGS_BEHAVIOUR_NOT_IMPORTANT, ENFORCE_ENCODING_NONE);
      if (ins.size != 0)
      {
        ot_check(ins);
      }
      else
      {
        ScratchRegAlloc off_sc = get_scratch_reg_with_save(excl | (1u << (uint32_t)buf_reg) | (1u << (uint32_t)base));
        load_full_const(off_sc.reg, PREG_NONE, abs_off, NULL);
        ot_check(sign ? th_sub_reg(buf_reg, base, off_sc.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
                                   ENFORCE_ENCODING_NONE)
                      : th_add_reg(buf_reg, base, off_sc.reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT,
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
  ot_check(th_mov_reg(R_IP, buf_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));

  /* Load resume address and saved SP into caller-saved regs first
   * (before we clobber r4+ with the restore) */
  ot_check(th_ldr_imm(0, R_IP, 36, 6, ENFORCE_ENCODING_NONE)); /* r0 = resume addr */
  ot_check(th_ldr_imm(1, R_IP, 32, 6, ENFORCE_ENCODING_NONE)); /* r1 = saved SP    */

  /* Restore callee-saved registers r4-r11 */
  ot_check(th_ldr_imm(4, R_IP, 0, 6, ENFORCE_ENCODING_NONE));     /* r4  = buf[0] */
  ot_check(th_ldr_imm(5, R_IP, 4, 6, ENFORCE_ENCODING_NONE));     /* r5  = buf[1] */
  ot_check(th_ldr_imm(6, R_IP, 8, 6, ENFORCE_ENCODING_NONE));     /* r6  = buf[2] */
  ot_check(th_ldr_imm(R_FP, R_IP, 12, 6, ENFORCE_ENCODING_NONE)); /* r7  = buf[3] (FP) */
  ot_check(th_ldr_imm(8, R_IP, 16, 6, ENFORCE_ENCODING_NONE));    /* r8  = buf[4] */
  ot_check(th_ldr_imm(9, R_IP, 20, 6, ENFORCE_ENCODING_NONE));    /* r9  = buf[5] */
  ot_check(th_ldr_imm(10, R_IP, 24, 6, ENFORCE_ENCODING_NONE));   /* r10 = buf[6] */
  ot_check(th_ldr_imm(11, R_IP, 28, 6, ENFORCE_ENCODING_NONE));   /* r11 = buf[7] */

  /* Restore SP */
  ot_check(th_mov_reg(R_SP, 1, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));

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
    ot_check(
        th_mov_reg(R_IP, fn_reg, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
  }

  /* Step 3: Restore r0-r3 from the args block.
   * Layout: [+0]=stack_args_ptr, [+4]=r0, [+8]=r1, [+12]=r2, [+16]=r3. */
  ot_check(th_ldr_imm(R0, args_reg, 4, 6, ENFORCE_ENCODING_NONE));
  ot_check(th_ldr_imm(R1, args_reg, 8, 6, ENFORCE_ENCODING_NONE));
  ot_check(th_ldr_imm(R2, args_reg, 12, 6, ENFORCE_ENCODING_NONE));
  ot_check(th_ldr_imm(R3, args_reg, 16, 6, ENFORCE_ENCODING_NONE));

  /* Step 4: Call the function via BLX R12.
   * This clobbers LR and r0-r3 (caller-saved). */
  ot_check(th_blx_reg(R_IP));

  /* Step 5: Move return value (r0) to dest register. */
  int dest_reg = mach_get_dest_reg(&ctx, &dest, 0);
  if (dest_reg != R0)
  {
    ot_check(
        th_mov_reg(dest_reg, R0, FLAGS_BEHAVIOUR_NOT_IMPORTANT, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false));
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
