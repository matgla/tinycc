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

/* Thumb-2 code generator: target state and ABI tables, the scratch-register
 * tracker, dry-run state, branch optimisation, the MOV-coalescing and
 * STR->LDR peephole caches, and the literal pool.  The rest of the generator
 * is in arm-thumb-{emit,alu,mem,fp,frame,call}.c; see arm-thumb-gen.h. */

#include "arm-thumb-gen.h"

static ScratchRegAlloc get_scratch_reg_for_sym_addr(Sym *raw_sym, int64_t imm, uint32_t exclude_regs);
static ScratchRegAlloc get_scratch_reg_for_const(int64_t value, uint32_t exclude_regs);
/* Forward declarations */
static void branch_opt_init(void);
static void branch_opt_analyze(uint32_t *ir_to_code_mapping, int mapping_size);

/* Workaround for TCC ARM ABI bugs:
 * 1. int64_t args miscount register pairs  2. 5th+ args not correctly pushed to stack
 * By passing sym through a file-scope global, load_full_const stays at 4 register args.
 * Set _lfc_sym before calling load_full_const; it is consumed and reset to NULL inside. */
struct Sym *_lfc_sym;

ThumbGeneratorState thumb_gen_state;

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
  call_layout.vfp_used = 0; /* AAPCS VFP: whole s0-s15 bank free */
  call_layout.vfp_exhausted = 0;
  call_layout.hard_float = out_layout->hard_float;
  call_layout.is_variadic = out_layout->is_variadic;

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
thumb_flags_behaviour flags_safe(void)
{
  if (tcc_state->ir && tcc_state->ir->codegen_flags_live)
    return FLAGS_BEHAVIOUR_BLOCK;
  return FLAGS_BEHAVIOUR_NOT_IMPORTANT;
}

/* Helper to validate a Sym pointer - returns NULL if invalid/unusable for relocation */
Sym *validate_sym_for_reloc(Sym *sym)
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

/* Reserved-namespace names only: predefining plain identifiers (`arm`,
 * `arm_elf`) breaks valid C11 code that uses them — the native bootstrap
 * died on a local variable named `arm`.  GCC only defines such unprefixed
 * macros in -std=gnu* mode, which tcc does not implement. */
ST_DATA const char *const target_machine_defs = "__arm__\0"
                                                "__arm\0"
                                                "__arm_elf__\0"
                                                "__arm_elf\0"
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
unsigned char sb_relative_got;

int offset_to_args = 0;

thumb_flags_behaviour g_setflags = FLAGS_BEHAVIOUR_SET;

uint32_t caller_saved_registers;
uint32_t pushed_registers;
int allocated_stack_size;
int epilogue_stack_dealloc;     /* total SUB SP amount to restore in epilogue (includes alignment pad) */
int vararg_push_size = 0;       /* bytes pushed for variadic r0-r3 save (16 or 0) */

/* Convert a local/spill frame offset (IR view: negative, counted down from
 * the top of the frame) into an offset from the frame base register.
 *
 * The base is SP, or the frame pointer, which the prologue sets to the SP it
 * leaves (the bottom of the frame, as gcc's r7 does) -- so the two agree:
 * locals occupy base+0 .. base+allocated_stack_size-1, and the alignment pad
 * sits above them, below the pushed registers:
 *     top + frame_offset = base + allocated_stack_size + frame_offset.
 * Offsets off the frame pointer are positive, so a Thumb load/store encodes
 * them directly up to 4095 (16-bit to 124).  A frame pointer at the top of
 * the frame, as before, only reached -255 without movw + rsb.  Off SP, a
 * scratch PUSH inside the current instruction has moved SP down: without the
 * bias every access in the push window reads/writes 4 bytes low per active
 * push (struct_byval fuzz seed 6105: LDR of a by-value field between
 * push {r0} and pop {r0}). */
int fp_adjust_local_offset(int frame_offset, int is_param)
{
  if (is_param || frame_offset > 0)
    return frame_offset;
  int off = allocated_stack_size + frame_offset;
  if (!tcc_state->need_frame_pointer)
    off += scratch_push_sp_bias();
  return off;
}

/* Offset from the frame base register (FP, or SP when there is none) of byte
 * `param_off` of the incoming stack parameters, which sit above the saved
 * registers (offset_to_args).  Off SP, a scratch PUSH active in the current
 * instruction has moved SP down, exactly as for a local (fp_adjust_local_offset):
 * without the bias a by-value parameter passed on through the memcpy path of
 * place_stack_arg_struct was copied from 24 bytes too low. */
int param_frame_offset(int param_off)
{
  int off = param_off + offset_to_args;
  if (!tcc_state->need_frame_pointer)
    off += scratch_push_sp_bias();
  return off;
}

/* Additional scratch register exclusions (e.g. to protect argument registers
 * while materializing an indirect call target). Applied on top of per-call
 * exclude masks. */
uint32_t scratch_global_exclude = 0;

/* Callee-saved register holding this function's .rodata runtime base for the
 * whole body, or -1 when the function addresses shared .rodata the long way.
 * Claimed per function by tcc_gen_machine_rodata_anchor_claim(); see the
 * commentary there. */
int rodata_anchor_reg = -1;

/* Registers no scratch picker may ever hand out in this function: R9 is the
 * GOT base under text/data separation, and the rodata anchor holds a value
 * with no interval behind it, so liveness always reports it dead. */
uint32_t scratch_exclude_baseline(void)
{
  uint32_t mask = text_and_data_separation ? (1u << R9) : 0;
  if (rodata_anchor_reg >= 0)
    mask |= 1u << rodata_anchor_reg;
  return mask;
}

/* Track registers that were PUSH'ed by get_scratch_reg_with_save() in ORDER.
 * We must POP in reverse order since ARM POP with register lists always pops
 * in register-number order, not stack order.
 * Size 128 since same register can be pushed multiple times for complex ops like
 * function calls with many arguments. */
static int scratch_push_stack[128];
int scratch_push_type[128]; /* 1 = PUSH, 2 = STR to scratch area */
int scratch_push_count = 0;

/* Flag: set to 1 when a real-run (non-dry-run) scratch PUSH is emitted.
 * Used by codegen to detect when FP omission caused SP-corrupting pushes
 * and trigger recompilation with FP enabled. */
static int real_run_scratch_push_detected = 0;

/* Tail-call flag: when set, the next gcall_or_jump_mop emits B (branch)
 * instead of BL (branch-with-link), and post-call cleanup is skipped. */
int tail_call_pending = 0;

/* Current slot index within the scratch save area (0-based).
 * Incremented on save, decremented on restore. */
static int scratch_save_slot = 0;

/* Debug tracking: current IR opcode being processed (set by codegen.c) */
int g_debug_current_op = -1;

/* Resolve the base register for a captured variable access.
 * For depth 1, returns R10 directly.
 * For depth > 1, emits LDR chain to follow ancestor frame pointers
 * and returns a scratch register holding the target ancestor's FP.
 * Caller must restore scratch via *out_scratch when done. */
int resolve_chain_base(TCCIRState *ir, int ci, uint32_t exclude_regs, ScratchRegAlloc *out_scratch,
                              int *used_scratch)
{
  int depth = ir->captured_chain_depths[ci];
  if (depth <= 1)
  {
    *used_scratch = 0;
    return architecture_config.static_chain_reg; /* R10 */
  }

  /* Multi-hop: follow chain through (depth - 1) intermediate frames.
   * Each frame saves its incoming R10 at frame offset -4 (CHAIN_SLOT_OFFSET),
   * i.e. 4 below the frame top that the chain points at. */
  *out_scratch = get_scratch_reg_with_save(exclude_regs);
  *used_scratch = 1;

  /* Start from R10 (points to immediate parent's frame top) */
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

/* Dispatch an imm_handler call through a direct call instead of an indirect
 * (function pointer) call.  Same workaround as thumb_call_reg_handler: the
 * cross-compiler miscompiles indirect calls that combine an sret return
 * (thumb_opcode is 8 bytes) with stack-passed arguments — the callee reads
 * garbage for the 5th/6th parameters (flags/enc), so e.g. the high-half SBCS
 * of a 64-bit CMP silently loses its S bit.  Comparing the pointer and
 * branching to a direct call makes the cross emit correct argument passing. */
thumb_opcode thumb_call_imm_handler(thumb_imm_handler_t fn, uint32_t rd, uint32_t rn, uint32_t imm,
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

/* Phase-3 per-instruction scratch constraint counters.
 * Incremented/set by mach_alloc_scratch(); reset and read via the
 * tcc_gen_machine_insn_scratch_*() public functions.
 * Declared here (before mach_alloc_scratch) to avoid a forward-reference to
 * dry_run_state which is defined later in the file. */
static int g_insn_scratch_allocs = 0;     /* total scratch allocs this instruction */
static uint16_t g_insn_scratch_saves = 0; /* registers that required PUSH this instruction */
int is_vfp_reg(int r) { return LS_IS_VFP_REG(r); }
int vfp_num(int r) { return LS_VFP_REG_NUM(r); }

/* Allocate a scratch register for the current instruction.
 * excl: bitmask of registers that must not be chosen.
 * The allocation is recorded in ctx so mach_release_all() can free it. */
int mach_alloc_scratch(MachineCodegenContext *ctx, uint32_t excl)
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

/* mach_alloc_scratch variant for a scratch that will receive &sym+imm (via
 * tcc_machine_load_constant): same ctx bookkeeping, but the register choice
 * goes through get_scratch_reg_for_sym_addr so a repeat materialization of
 * the same symbol address reuses the register still holding it (the
 * subsequent load then elides to zero instructions). */
int mach_alloc_scratch_for_sym(MachineCodegenContext *ctx, uint32_t excl, Sym *sym, int64_t imm)
{
  if (ctx->n_scratch >= MACH_CTX_MAX_SCRATCH)
    tcc_error("compiler_error: mach_alloc_scratch: per-instruction scratch limit exceeded");
  ScratchRegAlloc alloc = get_scratch_reg_for_sym_addr(sym, imm, excl);
  ctx->scratches[ctx->n_scratch++] = alloc;
  g_insn_scratch_allocs++;
  if (alloc.would_save)
    g_insn_scratch_saves |= (uint16_t)(1u << (unsigned)alloc.reg);
  return alloc.reg;
}

/* mach_alloc_scratch variant for a scratch that will receive a plain integer
 * constant: routes the register choice through get_scratch_reg_for_const so a
 * repeat materialization of the same literal reuses the register still holding
 * it (the subsequent load then elides to zero instructions). */
static int mach_alloc_scratch_for_const(MachineCodegenContext *ctx, uint32_t excl, int64_t value)
{
  if (ctx->n_scratch >= MACH_CTX_MAX_SCRATCH)
    tcc_error("compiler_error: mach_alloc_scratch: per-instruction scratch limit exceeded");
  ScratchRegAlloc alloc = get_scratch_reg_for_const(value, excl);
  ctx->scratches[ctx->n_scratch++] = alloc;
  g_insn_scratch_allocs++;
  if (alloc.would_save)
    g_insn_scratch_saves |= (uint16_t)(1u << (unsigned)alloc.reg);
  return alloc.reg;
}

/* Release all scratch registers allocated for the current instruction in
 * reverse (LIFO) order — required because ARM push/pop works by register
 * number, so the last-pushed register must be popped first. */
void mach_release_all(MachineCodegenContext *ctx)
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
/* A MACH_OP_SPILL operand with vreg < 0 is a local's own stack slot
 * (machine_op_from_ir, "concrete stack slots"), holding an object exactly as
 * wide as its btype; with a vreg it is that vreg's spill slot, a full register
 * image a word store wrote.  lea_fold turns `T = &local; ... *T` into such a
 * local slot, so a byte or halfword local reaches the value paths below.  Read
 * as a word, a u16 picked up its neighbour's bytes (Zig's `if (err.error)` on a
 * 2-byte error union took the wrong branch); written as a word, it would
 * clobber them. */
static int mach_is_narrow_local_slot(const MachineOperand *op)
{
  return op->kind == MACH_OP_SPILL && !op->needs_deref && op->vreg < 0 &&
         (op->btype == IROP_BTYPE_INT8 || op->btype == IROP_BTYPE_INT16);
}

/* A VAR the allocator spilled to a slot of its own: an integer spill slot is a
 * whole word nobody else shares, and its reads take the width of whichever
 * operand names it -- `CMP V,#0` reads a word even when V's def stored a byte.
 * Its narrow stores extend to a word like a TEMP's.  An address-taken VAR lives
 * in its home, which pointer stores write narrow, so it keeps narrow stores
 * (and mach_load_slot's narrow reads). */
int mach_var_owns_spill_slot(int vreg)
{
  if (vreg < 0 || TCCIR_DECODE_VREG_TYPE(vreg) != TCCIR_VREG_TYPE_VAR || !tcc_state->ir)
    return 0;
  IRLiveInterval *li = tcc_ir_get_live_interval(tcc_state->ir, vreg);
  return li && !li->addrtaken && !li->is_lvalue && !li->is_volatile && !li->is_struct;
}

/* Load a MACH_OP_SPILL slot's value (the slot itself, not through it).
 * A narrow local slot, or a VAR's, is read at its width, as the LOAD handler
 * reads it: a VAR whose address is taken keeps its vreg but lives in its
 * home, which a store through the pointer wrote a byte or halfword of --
 * read as a word, the rest came from whatever was there before (a u8 passed
 * to printf printed as aaaaaaa5).  A TEMP's or PARAM's slot only ever holds
 * a register image written as a word, and a word load is exact for it; its
 * operand's narrow type need not say how that image was extended.  A copy
 * into a loop temp first defined from a plain `char` keeps the char type
 * after `(unsigned)c` (a no-op in the IR), and reading the spilled
 * `cond * (unsigned)c` back with LDRSB turned 0xaf into 0xffffffaf
 * (212_fuzz_cprop_copy_into_loop_phi at -O0). */
void mach_load_slot(int dest_reg, const MachineOperand *op)
{
  if (op->needs_deref || (op->btype != IROP_BTYPE_INT8 && op->btype != IROP_BTYPE_INT16) ||
      (op->vreg >= 0 && TCCIR_DECODE_VREG_TYPE(op->vreg) != TCCIR_VREG_TYPE_VAR))
  {
    tcc_machine_load_spill_slot(dest_reg, op->u.spill.offset);
    return;
  }
  const int adj = fp_adjust_local_offset(op->u.spill.offset, 0);
  const int sign = (adj < 0), abs_off = sign ? -adj : adj;
  const uint32_t base = (uint32_t)(tcc_state->need_frame_pointer ? R_FP : R_SP);
  load_from_base(dest_reg, PREG_REG_NONE, op->btype, (int)op->is_unsigned, abs_off, sign, base);
}

/* Bytes mach_load_slot reads and mach_store_slot writes for a MACH_OP_SPILL
 * operand.  Kept next to the two functions whose choices they mirror. */
int mach_slot_load_width(const MachineOperand *op)
{
  if (op->needs_deref || (op->btype != IROP_BTYPE_INT8 && op->btype != IROP_BTYPE_INT16) ||
      (op->vreg >= 0 && TCCIR_DECODE_VREG_TYPE(op->vreg) != TCCIR_VREG_TYPE_VAR))
    return 4;
  return op->btype == IROP_BTYPE_INT8 ? 1 : 2;
}
int mach_slot_store_width(const MachineOperand *op)
{
  if (!mach_is_narrow_local_slot(op))
    return 4;
  return op->btype == IROP_BTYPE_INT8 ? 1 : 2;
}

/* Store a value into a MACH_OP_SPILL slot (the slot itself, not through it). */
static void mach_store_slot(int src_reg, const MachineOperand *op)
{
  if (!mach_is_narrow_local_slot(op))
  {
    tcc_machine_store_spill_slot(src_reg, op->u.spill.offset);
    return;
  }
  const int adj = fp_adjust_local_offset(op->u.spill.offset, 0);
  const int sign = (adj < 0), abs_off = sign ? -adj : adj;
  const uint32_t base = (uint32_t)(tcc_state->need_frame_pointer ? R_FP : R_SP);
  if (op->btype == IROP_BTYPE_INT8)
    th_store8_imm_or_reg(src_reg, base, abs_off, sign);
  else
    th_store16_imm_or_reg(src_reg, base, abs_off, sign);
}
int mach_ensure_in_reg(MachineCodegenContext *ctx, const MachineOperand *op, uint32_t excl)
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

  case MACH_OP_VFP_REG:
    /* Bridge a single-precision VFP register into a GPR for GPR-path consumers. */
    {
      int r = mach_alloc_scratch(ctx, excl);
      ot_check(th_vmov_gp_sp((uint16_t)r, (uint16_t)op->u.reg.r0, 1)); /* r = sN */
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
      /* Simple spill (or a local's own slot): load the value. */
      int r = mach_alloc_scratch(ctx, excl);
      mach_load_slot(r, op);
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
    if (op->needs_deref)
    {
      /* Absolute-address lvalue (`*(T *)0x40000000`) used as a value operand:
       * materialize the address, then read through it. */
      int addr_r = mach_alloc_scratch(ctx, excl);
      tcc_machine_load_constant(addr_r, PREG_REG_NONE, op->u.imm.val, 0, NULL);
      int r = mach_alloc_scratch(ctx, excl | (1u << (uint32_t)addr_r));
      load_from_base(r, PREG_REG_NONE, op->btype, (int)op->is_unsigned, 0, 0, (uint32_t)addr_r);
      return r;
    }
    int r = mach_alloc_scratch_for_const(ctx, excl, op->u.imm.val);
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
      int r = mach_alloc_scratch_for_sym(ctx, excl, sym, op->u.sym.addend);
      tcc_machine_load_constant(r, PREG_REG_NONE, op->u.sym.addend, 0, sym);
      return r;
    }
    else
    {
      /* Load symbol address into a scratch base reg, then dereference. */
      int r = mach_alloc_scratch(ctx, excl);
      int base = mach_alloc_scratch_for_sym(ctx, excl | (1u << (uint32_t)r), sym, 0);
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
    const int adjusted = param_frame_offset(op->u.param.offset);
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

/* The registers a register operand reads where it sits: exclude them when
 * another operand of the same instruction is materialised first.  A scratch
 * is taken from the live registers when none is free (saved around the use),
 * so a scratch not told about the base could be the base itself:
 * `str r0,[sp]; movs r0,#0; str r0,[r0,#64]` for `*(P0 + 64) = 0` in a
 * function whose r0-r3 all hold parameters (pr108498-1 after sret_nrvo). */
uint32_t mach_reg_excl(const MachineOperand *op)
{
  if (op->kind != MACH_OP_REG)
    return 0;
  uint32_t m = 0;
  if (thumb_is_hw_reg(op->u.reg.r0))
    m |= 1u << (uint32_t)op->u.reg.r0;
  if (op->is_64bit && thumb_is_hw_reg(op->u.reg.r1))
    m |= 1u << (uint32_t)op->u.reg.r1;
  return m;
}

/* Try to emit an immediate-form instruction for src2; if the encoding succeeds,
 * sets *imm_emitted=true and returns PREG_REG_NONE.  Otherwise loads src2 into
 * a scratch register and returns it (like mach_ensure_in_reg). */
int mach_ensure_imm_or_reg(MachineCodegenContext *ctx, const MachineOperand *op, uint32_t excl,
                                  thumb_imm_handler_t imm_handler, int dest_reg, int src1_reg,
                                  thumb_flags_behaviour flags, bool *imm_emitted)
{
  *imm_emitted = false;
  /* An lvalue immediate is an ADDRESS to read through, not an operand value. */
  if (op->kind == MACH_OP_IMM && !op->needs_deref && imm_handler)
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
int mach_get_dest_reg(MachineCodegenContext *ctx, const MachineOperand *op, uint32_t excl)
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

  case MACH_OP_VFP_REG:
  case MACH_OP_SPILL:
  case MACH_OP_FRAME_ADDR:
  case MACH_OP_PARAM_STACK:
  case MACH_OP_CHAIN_REL:
  case MACH_OP_SYMBOL:
    /* VFP dest: compute in a GPR scratch; mach_writeback_dest bridges it to sN. */
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
void mach_writeback_dest(const MachineOperand *op, int reg)
{
  if (!op || op->kind == MACH_OP_NONE)
    return;

  switch (op->kind)
  {
  case MACH_OP_VFP_REG:
    ot_check(th_vmov_gp_sp((uint16_t)reg, (uint16_t)op->u.reg.r0, 0)); /* sN = reg */
    break;

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
    mach_store_slot(reg, op);
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
    ScratchRegAlloc rr = get_scratch_reg_for_sym_addr(sym, 0, excl);
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
      mach_load_slot(dest_reg, op);
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
     * Use get_scratch_reg_for_sym_addr for the base so it won't clobber
     * dest_reg (and reuses a register already holding the address). */
    {
      uint32_t excl = (1u << (uint32_t)dest_reg);
      ScratchRegAlloc base_alloc = get_scratch_reg_for_sym_addr(sym, 0, excl);
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
    const int adjusted = param_frame_offset(op->u.param.offset);
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
CodeGenDryRunState dry_run_state;
/* Rehearsal mode: still a dry run (ot() writes no bytes) but every decision is
 * made exactly as the real pass would make it, so the resulting address map is
 * a faithful size model.  The discovery dry run cannot be one: where it finds
 * no free scratch register it merely RECORDS the push and emits nothing, and
 * its finalisation then reassigns registers and resizes the frame — so its
 * layout is not the layout the real pass produces. */
int dry_run_rehearsal = 0;
ST_FUNC void tcc_gen_machine_dry_run_set_rehearsal(int on) { dry_run_rehearsal = on; }

/* Mapping-symbol emission, gated to the real pass.  Dry passes advance `ind`
 * with drifted offsets (branches emit wide), so a symbol emitted there points
 * mid-pool or mid-code and actively misleads objdump — that is exactly the
 * bug that made the ungated th_sym_* calls worse than useless. */
void map_sym_d(void)
{
  if (dry_run_state.active || nocode_wanted)
    return;
  th_sym_d();
}
void map_sym_t(void)
{
  if (dry_run_state.active || nocode_wanted)
    return;
  th_sym_t();
}

/* Bytes the real run's scratch PUSHes have currently moved SP below its
 * steady-state position.  Derived from the push bookkeeping so it can never
 * drift from the actual PUSH/POP pairing (including deferred pops).  The dry
 * run never emits pushes, so its bias is always 0. */
/* Bytes an explicit register save around a helper call has moved SP (see
 * place_stack_arg_struct's memcpy path).  Emitted, and so counted, in the dry
 * run as in the real one. */
int helper_call_sp_bias;

/* Bytes a call's own argument window has dropped SP below the frame's
 * reserved outgoing area (tcc_gen_machine_func_call_mop).  Part of every
 * frame-relative SP offset, like the pushes; NOT of an offset into the
 * argument area, which is relative to the moved SP. */
int call_args_sp_bias;
int scratch_push_sp_bias(void)
{
  int bias = helper_call_sp_bias + call_args_sp_bias;
  if (dry_run_state.active)
    return bias;
  for (int i = 0; i < scratch_push_count; i++)
    if (scratch_push_type[i] == 1)
      bias += 4;
  return bias;
}

/* Separate literal pool for dry-run mode to avoid modifying the real pool.
 * This allows accurate code size tracking without affecting the real pass. */
ThumbLiteralPoolEntry *dry_run_literal_pool = NULL;
int dry_run_literal_pool_count = 0;

/* Monotonic per-function totals (never reset by a pool flush) used by the
 * forward-branch narrowing safety check: the rehearsal records them per IR
 * instruction so the real pass can bound the pool pressure over a branch's
 * range and prove no flush can land inside it. */
static int pool_flushes_total = 0;
int pool_entries_total = 0;
ST_FUNC int tcc_gen_machine_pool_flushes_total(void) { return pool_flushes_total; }
ST_FUNC int tcc_gen_machine_pool_entries_total(void) { return pool_entries_total; }

/* A 16-bit NOP so the following instruction starts on a word boundary.  `o()`
 * carries the literal-pool window along (the window is measured from `ind`),
 * and a NOP writes no register and touches no memory, so none of ot()'s
 * encoder caches need invalidating -- the loop head resets them anyway. */
/* The largest pad tcc_gen_machine_align_branch_target can emit (align - 2
 * bytes; 2 at the default word alignment), for the branch-narrowing bounds. */
int align_pad_max;

ST_FUNC void tcc_gen_machine_align_branch_target(int align)
{
  if (align < 2)
    return;
  if (align - 2 > align_pad_max)
    align_pad_max = align - 2;
  while (ind & (align - 1))
    o(0xbf00);
}
int dry_run_literal_pool_size = 0;
TCCChainedHash literal_pool_hash;
LiteralPoolLookupCache literal_pool_last_lookup;
uint32_t literal_pool_hash_func(Sym *sym, int64_t imm)
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
int literal_pool_lookup_cache_find(LiteralPoolLookupCache *cache, uint32_t full_hash, Sym *sym,
                                                 int64_t imm)
{
  LiteralPoolLookupCacheEntry *entry = &cache->entries[full_hash & (LITERAL_POOL_LOOKUP_CACHE_SIZE - 1)];
  if (entry->valid && entry->hash == full_hash && entry->sym == sym && entry->imm == imm)
    return entry->pool_index;
  return -1;
}
void literal_pool_lookup_cache_insert(LiteralPoolLookupCache *cache, uint32_t full_hash, Sym *sym,
                                                    int64_t imm, int pool_index)
{
  LiteralPoolLookupCacheEntry *entry = &cache->entries[full_hash & (LITERAL_POOL_LOOKUP_CACHE_SIZE - 1)];
  entry->sym = sym;
  entry->imm = imm;
  entry->pool_index = pool_index;
  entry->hash = full_hash;
  entry->valid = 1;
}
int literal_pool_hash_find(TCCChainedHash *hash, ThumbLiteralPoolEntry *pool, uint32_t full_hash,
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
void literal_pool_hash_insert(TCCChainedHash *hash, uint32_t full_hash, int pool_index)
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

static ThumbGenStateSnapshot dry_run_snapshot;

static void thumb_gen_state_snapshot_save(ThumbGenStateSnapshot *snap)
{
  snap->code_size = thumb_gen_state.code_size;
  snap->literal_pool_count = thumb_gen_state.literal_pool_count;
  snap->literal_pool_size = thumb_gen_state.literal_pool_size;
  snap->pool_window_first = thumb_gen_state.pool_window_first;
  snap->pool_bytes = thumb_gen_state.pool_bytes;
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
  thumb_gen_state.pool_window_first = snap->pool_window_first;
  thumb_gen_state.pool_bytes = snap->pool_bytes;
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

static BranchOptState branch_opt_state;

/* Check if offset fits in 16-bit conditional branch (T1 encoding)
 * Range: -256 to +254 bytes (imm8 * 2), must be even */
int branch_fits_t1(int offset)
{
  return (offset >= -256 && offset <= 254 && (offset & 1) == 0);
}

/* Check if offset fits in 16-bit unconditional branch (T2 encoding)
 * Range: -2048 to +2046 bytes (imm11 * 2), must be even */
int branch_fits_t2(int offset)
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
  thumb_gen_state.pool_window_first = -1;
  thumb_gen_state.pool_bytes = 0;
  thumb_gen_state.cached_global_sym = NULL;
  thumb_gen_state.cached_global_reg = PREG_NONE;
  thumb_gen_state.function_argument_count = 0;
  pool_flushes_total = 0;
  pool_entries_total = 0;
  /* call_sites_by_id - don't modify, just track that we saved it */
  imm_cache_reset_all();
}

ST_FUNC void tcc_gen_machine_dry_run_end(void)
{
  dry_run_state.active = 0;
  /* The real pass re-counts from zero so its running totals line up with the
   * per-instruction snapshots the rehearsal recorded. */
  pool_flushes_total = 0;
  pool_entries_total = 0;
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

/* Max nested-call save slots any call site used during the dry run — the
 * exact demand the static max_nested_save_regs reservation over-approximates. */
ST_FUNC int tcc_gen_machine_dry_run_get_max_nested_saves(void)
{
  return dry_run_state.max_nested_saves;
}

ST_FUNC void tcc_gen_machine_rodata_anchor_reset(void)
{
  rodata_anchor_reg = -1;
}

ST_FUNC int tcc_gen_machine_rodata_anchor_get(void)
{
  return rodata_anchor_reg;
}

/* Claim a callee-saved register to hold this function's .rodata runtime base.
 *
 * A shared-.rodata address is anchor + link-time offset, and the anchor lives
 * in a GOT slot, so every reference has to load it.  Done per reference that
 * costs a scratch register nobody has:
 *   push {tmp}; ldr.w tmp,[r9,#24]; add r,r,tmp; pop {tmp}    (10 bytes)
 * Two earlier attempts tried to drop the push/pop by proving some register was
 * free at the site.  Both miscompiled, because load_full_const is reached from
 * compound emissions (block copies, 64-bit STRD marshalling) that hold values
 * in registers by hand, with no interval behind them — liveness reports those
 * registers dead and hands one straight back.
 *
 * Hoisting removes the question.  The anchor is loaded once in the prologue
 * into a register the allocator did not use, and each reference is then a
 * bare `add r,r,anchor` — 2 bytes, no scratch, so there is nothing for a
 * hand-held value to collide with.  The register is excluded from every
 * scratch picker for the whole body (scratch_exclude_baseline), which also
 * keeps it out of the two pools that hand out prologue-pushed registers on
 * the grounds that the epilogue restores them.
 *
 * Called from the discovery run's finalisation, so `used_registers` is the
 * settled allocation and the decision precedes both the prologue and the two
 * passes that model it — dry and real always agree on the encoding.  Being
 * wrong in either direction only costs bytes: an unclaimed function keeps the
 * push/pop form, and a claimed one that turns out to reference nothing wastes
 * a prologue load.  Returns the mask to add to the prologue push list. */
ST_FUNC uint32_t tcc_gen_machine_rodata_anchor_claim(uint64_t used_registers)
{
  rodata_anchor_reg = -1;

  if (!tcc_state->share_rodata || !text_and_data_separation)
    return 0;

  /* One reference pays for the prologue load and its push/pop at best; two
   * clear it.  (Per-function counts are heavily skewed — most functions that
   * touch .rodata at all touch it several times.) */
  if (dry_run_state.rodata_anchor_sites < 2)
    return 0;

  TCCIRState *ir = tcc_state->ir;
  if (!ir || ir->naked)
    return 0; /* no prologue to hoist into, so nothing saves the register */

  /* Inline asm can name a register outright, which neither the allocator nor
   * used_registers can see.  Same reservation the frame pointer makes. */
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    const int op = ir->compact_instructions[i].op;
    if (op == TCCIR_OP_ASM_INPUT || op == TCCIR_OP_INLINE_ASM || op == TCCIR_OP_ASM_OUTPUT)
      return 0;
  }

  /* R7 is the frame base whenever the prologue decides it needs one, and that
   * decision is made after this point.  R10 is the static chain register.  R11
   * and R12 are the backend's permanent scratch pair (they are deliberately
   * not sticky in scratch_global_exclude, so excluding them here would not
   * hold).  What is left is R4-R6 and R8, and the low three come first: they
   * keep PUSH/POP in the 16-bit encoding, so a function that already saves a
   * low register pays nothing at all for the save. */
  static const int candidates[] = {R4, R5, R6, R8};
  uint32_t taken = (uint32_t)used_registers | scratch_global_exclude;
  for (unsigned i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++)
  {
    const int r = candidates[i];
    if (taken & (1u << r))
      continue;
    rodata_anchor_reg = r;
    /* Sticky for the whole body: the reset points rebuild the exclude mask
     * from scratch_exclude_baseline(), but the real pass can start without
     * passing through one of them (-O0 skips the rehearsal). */
    scratch_global_exclude |= 1u << r;
    return 1u << r;
  }
  return 0;
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
  scratch_global_exclude = scratch_exclude_baseline();
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

/* Prolog-pushed reg from `mask`, dead at the current instruction — usable as
 * scratch for free (prolog/epilog hides the clobber).  Real-run only:
 * pushed_registers is not valid during dry-run. */
static int scratch_pushed_dead_reg(TCCIRState *ir, uint32_t exclude_regs, uint32_t mask)
{
  if (DRY_RUN_MODELLING || !pushed_registers)
    return PREG_NONE;
  uint32_t reserved = (1u << R_FP) | scratch_exclude_baseline();
  if (tcc_state->text_and_data_separation)
    reserved |= (1u << 9);
  uint32_t live = tcc_ls_compute_live_regs(&ir->ls, ir->codegen_instruction_idx);
  if (ir->ls.live_regs_by_instruction && ir->codegen_instruction_idx >= 0 &&
      ir->codegen_instruction_idx < ir->ls.live_regs_by_instruction_size)
    live |= ir->ls.live_regs_by_instruction[ir->codegen_instruction_idx];
  uint32_t candidate = pushed_registers & mask & ~exclude_regs & ~live & ~reserved;
  return candidate ? (int)__builtin_ctz(candidate) : PREG_NONE;
}

/* Get a free scratch register using liveness information.
 * exclude_regs is a bitmap of registers that must not be used.
 * If no free register is found, saves R_IP to stack and returns it.
 * Returns ScratchRegAlloc with the register and whether it was saved.
 */
ScratchRegAlloc get_scratch_reg_with_save(uint32_t exclude_regs)
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
      /* ip/lr force 32-bit encodings; a pushed r4-r7 dead here is free AND narrow */
      if (reg == R_IP || reg == R_LR) {
        int low = scratch_pushed_dead_reg(ir, exclude_regs, 0x00F0u);
        if (low != PREG_NONE)
          reg = low;
      }
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
    if (reg == PREG_NONE)
    {
      int sreg = scratch_pushed_dead_reg(ir, exclude_regs, 0x0FF0u);
      if (sreg != PREG_NONE)
      {
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
  if (DRY_RUN_MODELLING)
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
   * of PUSH, to avoid moving SP (which would break SP-relative addressing).
   * Only while the slot is in STR's immediate range: in a frame of more than
   * 4 KB it may not be, and with no free register there is nothing to build
   * the address in -- then PUSH, which scratch_push_sp_bias accounts for
   * (pr28982b's 256 KB frame at -O1/-O2). */
  int save_sp_offset = 0;
  if (!tcc_state->need_frame_pointer && ir && ir->scratch_save_size > 0 &&
      scratch_save_slot < (ir->scratch_save_size / 4))
    save_sp_offset = allocated_stack_size + scratch_push_sp_bias() + ir->scratch_save_base + (scratch_save_slot * 4);
  if (!tcc_state->need_frame_pointer && ir && ir->scratch_save_size > 0 &&
      scratch_save_slot < (ir->scratch_save_size / 4) && save_sp_offset >= 0 && save_sp_offset <= 4095)
  {
    int sp_offset = save_sp_offset;
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
void restore_scratch_reg(ScratchRegAlloc *alloc)
{
  if (alloc->saved)
    imm_cache_invalidate_reg(alloc->reg);
  /* Dry run: don't emit pop, just update tracking */
  if (DRY_RUN_MODELLING)
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
    int sp_offset = allocated_stack_size + scratch_push_sp_bias() + frame_offset;
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
void restore_all_pushed_scratch_regs(void)
{
  /* Dry run: don't emit pops, just reset tracking */
  if (DRY_RUN_MODELLING)
  {
    scratch_push_count = 0;
    scratch_save_slot = 0;
    scratch_global_exclude = scratch_exclude_baseline();
    return;
  }

  /* Restore in reverse order.  scratch_push_count is trimmed as each entry
   * is restored so scratch_push_sp_bias() sees only the still-active pushes
   * while emitting the LDRs below. */
  for (int i = scratch_push_count - 1; i >= 0; i--)
  {
    int reg = scratch_push_stack[i];
    int type = scratch_push_type[i];
    LOG_SCRATCH("auto-restoring r%d (push order %d, type %d)", reg, i, type);
    scratch_push_count = i;
    if (type == 2)
    {
      /* Saved to scratch area: restore via LDR */
      TCCIRState *ir = tcc_state->ir;
      if (scratch_save_slot > 0)
        scratch_save_slot--;
      int frame_offset = ir->scratch_save_base + (scratch_save_slot * 4);
      int sp_offset = allocated_stack_size + scratch_push_sp_bias() + frame_offset;
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
  scratch_global_exclude = scratch_exclude_baseline();
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
int mov_equiv_it_pending;
ImmCacheEntry imm_cache[16];
void imm_cache_reset_all(void)
{
  for (int i = 0; i < 16; i++)
  {
    imm_cache[i].valid = 0;
    imm_cache[i].sym = NULL;
  }
}
void imm_cache_invalidate_reg(int reg)
{
  if (reg >= 0 && reg < 16)
    imm_cache[reg].valid = 0;
}

/* Scratch selection for a SYMBOL-ADDRESS materialization (the chosen register
 * will receive &sym+imm via tcc_machine_load_constant / load_full_const).
 *
 * Two placement rules close most of the duplicate literal-pool-load gap
 * (census: 11k duplicate same-literal `ldr [pc]` loads vs GCC's 5k):
 *
 *  1. If a free register ALREADY holds &sym+imm (per imm_cache), hand THAT
 *     register out — load_full_const's reuse check then elides the load
 *     entirely, so a repeat materialization costs zero instructions.
 *  2. On a miss, park the address in the HIGHEST free register of R0-R3.
 *     Every other scratch user (spill reloads, marshaling) allocates
 *     lowest-first, so a symbol base placed in R3 survives the R0 churn and
 *     turns the NEXT materialization into case 1.  (An unrolled
 *     table-indexed kernel — mibench_rijndael's encrypt — reloaded the same
 *     table base 14x through R0 precisely because the reload target was
 *     also every other user's first-choice scratch.)
 *
 * Both decisions read only imm_cache and liveness, which are maintained
 * identically in the dry and real passes (the invariant the elide path in
 * load_full_const already relies on), so dry/real code sizes stay in sync.
 * Every other case falls back to get_scratch_reg_with_save. */
int try_scratch_reg_for_sym_addr(Sym *raw_sym, int64_t imm, uint32_t exclude_regs, ScratchRegAlloc *out)
{
  TCCIRState *ir = tcc_state->ir;
  Sym *vsym = raw_sym ? validate_sym_for_reloc(raw_sym) : NULL;
  if (vsym && ir && thumb_gen_state.generating_function)
  {
    /* Live set at this instruction: the same union tcc_ls_find_free_scratch_reg
     * builds (per-instruction bitmap ∪ interval scan, via the LS cache). */
    uint32_t live = exclude_regs | scratch_global_exclude | (1u << R_SP) | (1u << R_PC);
    if (ir->leaffunc)
      live |= (1u << R_LR);
    LSLiveIntervalState *ls = &ir->ls;
    int idx = ir->codegen_instruction_idx;
    if (ls->live_regs_by_instruction && idx >= 0 && idx < ls->live_regs_by_instruction_size)
      live |= ls->live_regs_by_instruction[idx];
    if (ls->cached_instruction_idx == idx)
      live |= ls->cached_live_regs;
    else
    {
      uint32_t computed = tcc_ls_compute_live_regs(ls, idx);
      ls->cached_instruction_idx = idx;
      ls->cached_live_regs = computed;
      live |= computed;
    }
    /* 1) Reuse a free register that already holds &sym+imm. */
    for (int r = 0; r < 16; r++)
    {
      if (!(live & (1u << r)) && imm_cache[r].valid && imm_cache[r].sym == vsym &&
          imm_cache[r].value == imm)
      {
        ScratchRegAlloc res = {0};
        res.reg = r;
        scratch_global_exclude |= (1u << r);
        *out = res;
        return 1;
      }
    }
    /* 2) Miss: highest free low register, away from the lowest-first churn. */
    {
      uint32_t avail_low = (~live) & 0xFu;
      if (avail_low)
      {
        ScratchRegAlloc res = {0};
        res.reg = 31 - __builtin_clz(avail_low);
        scratch_global_exclude |= (1u << (uint32_t)res.reg);
        *out = res;
        return 1;
      }
    }
  }
  return 0;
}

static ScratchRegAlloc get_scratch_reg_for_sym_addr(Sym *raw_sym, int64_t imm, uint32_t exclude_regs)
{
  ScratchRegAlloc res;
  if (try_scratch_reg_for_sym_addr(raw_sym, imm, exclude_regs, &res))
    return res;
  return get_scratch_reg_with_save(exclude_regs);
}

/* Scratch selection for a PLAIN-CONSTANT materialization (the chosen register
 * will receive `value` via tcc_machine_load_constant).
 *
 * Same rule 1 as get_scratch_reg_for_sym_addr, for integer literals: when a
 * free register still holds `value`, hand THAT register out and the early-out
 * in tcc_machine_load_constant makes the materialization cost zero
 * instructions.  Register-saturated straight-line code re-materializes the
 * same literal constantly -- a rolling-hash chain reloaded its multiplier from
 * the literal pool once per round (252_fuzz: 143 pool loads for 64 distinct
 * words) because the chain's own values kept every scratch churning.
 *
 * Rule 2 of the symbol path (park high, away from the lowest-first churn) is
 * deliberately NOT copied: a literal, unlike a global's base address, is
 * usually consumed once right where it is produced, and parking it high only
 * displaces the spill traffic that wants those registers.
 *
 * The decision reads only imm_cache and liveness, both maintained identically
 * in the dry and real passes, so code sizes stay in sync. */
static ScratchRegAlloc get_scratch_reg_for_const(int64_t value, uint32_t exclude_regs)
{
  TCCIRState *ir = tcc_state->ir;
  if (ir && thumb_gen_state.generating_function)
  {
    uint32_t live = exclude_regs | scratch_global_exclude | (1u << R_SP) | (1u << R_PC);
    if (ir->leaffunc)
      live |= (1u << R_LR);
    LSLiveIntervalState *ls = &ir->ls;
    int idx = ir->codegen_instruction_idx;
    if (ls->live_regs_by_instruction && idx >= 0 && idx < ls->live_regs_by_instruction_size)
      live |= ls->live_regs_by_instruction[idx];
    if (ls->cached_instruction_idx == idx)
      live |= ls->cached_live_regs;
    else
    {
      uint32_t computed = tcc_ls_compute_live_regs(ls, idx);
      ls->cached_instruction_idx = idx;
      ls->cached_live_regs = computed;
      live |= computed;
    }
    for (int r = 0; r < 16; r++)
    {
      if (!(live & (1u << r)) && imm_cache[r].valid && imm_cache[r].sym == NULL &&
          imm_cache[r].value == value)
      {
        ScratchRegAlloc res = {0};
        res.reg = r;
        scratch_global_exclude |= (1u << r);
        return res;
      }
    }
  }
  return get_scratch_reg_with_save(exclude_regs);
}
void mov_equiv_reset_all(void)
{
  for (int i = 0; i < 16; i++)
    mov_equiv[i] = (uint8_t)i;
  mov_equiv_it_pending = 0;
}

/* Decode the IT instruction (Thumb-2 16-bit, opcode 0xBF<cond><mask>) and
 * return the number of instructions that will execute conditionally after
 * it — 1..4 depending on which bit of the mask is lowest-set.  Returns 0
 * when the opcode is not an IT (mask == 0 is a plain NOP/hint). */
int mov_equiv_it_block_length(thumb_opcode op)
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
void mov_equiv_invalidate_reg(int reg)
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
void mov_equiv_record_mov(int rd, int rm)
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
/* Count of conditioned instructions still pending inside an IT/ITE/... block,
 * tracked by ot().  Kept separate from mov_equiv_it_pending, which
 * mov_equiv_reset_all() may zero mid-block.
 *
 * Two things must not happen while this is non-zero, both because an IT's
 * condition mask covers a FIXED number of following instructions:
 *
 *  - a literal-pool flush: it emits its pool + B.W skip-branch BEFORE the
 *    bytes of the op being emitted, so the branch would occupy a conditioned
 *    slot, inherit the IT condition, and the opposite arm would fall through
 *    into pool data and execute it (fuzz ptr seed 5759: O2 HardFault);
 *  - eliding an instruction: dropping one shifts every later instruction up a
 *    slot, so the next unconditional instruction is swallowed INTO the block
 *    and the remaining ones take the wrong condition.
 *
 * ot() decrements it as each conditioned op is emitted, so on entry to the
 * helpers below a non-zero value means "the op about to be emitted is inside
 * an IT block". */
int pool_flush_it_pending;
int ot_check_mov_reg(uint32_t rd, uint32_t rm, thumb_flags_behaviour flags, thumb_shift shift,
                            thumb_enforce_encoding enc, bool in_it)
{
  /* `in_it` is what the caller believes; pool_flush_it_pending is what ot()
   * actually tracked.  Honour both -- a caller that forgets the flag would
   * corrupt the block exactly as an elided conditional LDR does. */
  const int coalesceable = (flags != FLAGS_BEHAVIOUR_SET) && !in_it && (pool_flush_it_pending == 0) &&
                           (shift.type == THUMB_SHIFT_NONE) && (rd < 16) &&
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
int decode_mov_reg_plain(thumb_opcode op, int *rd_out, int *rm_out)
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
static StrLdrCacheEntry strldr_cache[STRLDR_CACHE_CAPACITY];
static int strldr_cache_count;
/* Off for a function that touches volatile ANYWHERE.  Resetting per IR op is
 * not enough: `return *p + *p` on a `volatile int *` is two reads inside ONE
 * op, and both have to reach memory.  Volatile bodies are rare, so the whole
 * function pays rather than the analysis getting clever. */
static int strldr_cache_off;
FrameWordAccess frame_word_last;

ST_FUNC void tcc_gen_machine_strldr_cache_reset(void)
{
  strldr_cache_count = 0;
  /* Every point where another path can arrive (IR jump targets, backend
   * labels, asm, setjmp) resets this cache, so it is also where a pending
   * frame-word pair has to be forgotten. */
  frame_word_last.kind = 0;
}

/* Knob for strldr_cache_str_is_redundant alone: TCC_DISABLE_PASS=codegen:str_elide. */
static int strldr_str_elide_off;

ST_FUNC void tcc_gen_machine_str_elide_set_enabled(int enabled)
{
  strldr_str_elide_off = !enabled;
}

/* Knob for the same-slot ASSIGN skip in tcc_gen_machine_assign_mop_ex:
 * TCC_DISABLE_PASS=codegen:slot_self_copy.  Also off in a function with a
 * volatile access, where a copy of a value onto itself may still have to
 * touch memory. */
int slot_self_copy_off;

ST_FUNC void tcc_gen_machine_slot_self_copy_set_enabled(int enabled)
{
  slot_self_copy_off = !enabled;
}

ST_FUNC void tcc_gen_machine_strldr_cache_set_enabled(int enabled)
{
  strldr_cache_off = !enabled;
  strldr_cache_count = 0;
}

/* A backend-internal branch target: code below is reachable from somewhere
 * other than the preceding instruction, so no emission-order fact survives.
 * There are exactly three such points (one `gsym` and the two setjmp resume
 * labels); every other merge is an IR-level jump target that codegen.c
 * already resets at.  Needed because a plain branch no longer resets the
 * memory cache on its own -- see thumb_op_is_plain_branch. */
void codegen_internal_merge_point(void)
{
  mov_equiv_reset_all();
  tcc_gen_machine_strldr_cache_reset();
  imm_cache_reset_all();
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
void strldr_cache_invalidate_reg(int reg)
{
  for (int i = 0; i < strldr_cache_count; i++)
  {
    StrLdrCacheEntry *e = &strldr_cache[i];
    if (e->valid && (e->rt == reg || e->rn == reg))
      e->valid = 0;
  }
}

/* Drop every entry a write of `width` bytes at [rn + imm] could reach.
 *
 * Two accesses are provably distinct only when they name the same base
 * register and their byte ranges do not overlap: different base registers may
 * hold the same address (a pointer into the frame aliases SP), so a store
 * through one of them has to clear the other's entries.  A write to `rn`
 * itself is handled separately by strldr_cache_invalidate_reg, which is what
 * makes "same base register" mean "same address" here. */
void strldr_cache_invalidate_mem(int rn, int imm, int width)
{
  for (int i = 0; i < strldr_cache_count; i++)
  {
    StrLdrCacheEntry *e = &strldr_cache[i];
    if (!e->valid)
      continue;
    if (e->rn == rn && (e->imm + (int)e->width <= imm || imm + width <= e->imm))
      continue;
    e->valid = 0;
  }
}

/* Record that `rt` holds [rn + imm] after this access.  True of a store and of
 * a load alike -- which is the point: a spilled value materialized twice in a
 * row is two LOADS, not a store and a load, so a store-only cache never sees
 * the shape at all.
 *
 * Only 4-byte accesses are recorded.  A sub-word access does not leave the
 * register holding the memory: `strb rt,[rn,#4]` writes rt's low byte and
 * `ldrb rt,[rn,#4]` brings it back zero-extended, and the T1 STRB imm5 is
 * UNSCALED where the T1 STR imm5 is word-scaled, so the two decode to the same
 * (rt, rn, imm, size) triple and would match each other. */
void strldr_cache_record_access(int rt, int rn, int imm, uint32_t puw, int size, int width, int is_store)
{
  if (puw != 6)
  {
    tcc_gen_machine_strldr_cache_reset();
    return;
  }
  if (is_store)
    strldr_cache_invalidate_mem(rn, imm, width);
  if (width != 4)
    return;
  /* `ldr r0,[r0]` overwrites its own base: afterwards r0 is the loaded VALUE,
   * not the address it was loaded from, so "r0 holds [r0+0]" describes a slot
   * that no longer exists.  Recording it collapses a pointer-chase --
   * `ldr r0,[r0]; ldr r0,[r0]` for `**pp` -- into a single dereference.
   * A store cannot hit this: it writes no register. */
  if (!is_store && rt == rn)
    return;
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
  e->width = (uint8_t)width;
}

/* Return 1 when a matching unclobbered STR entry exists that makes this
 * LDR redundant.  Matches on all fields so a 16-bit LDR won't be elided
 * against a 32-bit STR (and vice versa) — the encodings might pick
 * different scale semantics. */
int strldr_cache_try_match_ldr(int rt, int rn, int imm, uint32_t puw, int size, int width)
{
  if (strldr_cache_off || puw != 6 || width != 4)
    return 0;
  /* Never answer inside an IT block.  An IT's mask conditions a FIXED number
   * of following instructions, so eliding one pulls the next unconditional
   * instruction into the block and shifts every remaining condition by a
   * slot.  The shape this was found on:
   *
   *      str    r0,[sp,#468]        str     r0,[sp,#468]
   *      ite    ne                  ite     ne
   *      ldrne  r0,[sp,#468]   ->   movne.w r0,#0          <- was the eq arm
   *      moveq.w r0,#0              ldreq.w r2,[sp,#1148]  <- swallowed in
   *      ldr.w  r2,[sp,#1148]
   *
   * leaving r2 uninitialised on the ne path.  The guard belongs here rather
   * than at the call sites because all of them -- ot_check_ldr_imm,
   * load_word_from_base (which every spill reload takes) and the LDRD pair
   * check -- ask through this one function.  Same reasoning as the
   * literal-pool flush suppression that pool_flush_it_pending was added for. */
  if (pool_flush_it_pending != 0)
    return 0;
  for (int i = 0; i < strldr_cache_count; i++)
  {
    StrLdrCacheEntry *e = &strldr_cache[i];
    if (!e->valid)
      continue;
    if (e->rt == rt && e->rn == rn && e->imm == imm && e->puw == puw && e->size == size &&
        e->width == width)
      return 1;
  }
  return 0;
}

/* A word STR of a register that already holds [rn + imm]: the memory has that
 * value, so the store writes nothing new.  This is the tail of a copy between
 * two values frame layout or the allocator put on the same slot --
 * `ldr r0,[sp,#N]; str r0,[sp,#N]`.  Same entry rules as the reload check
 * (4-byte, no writeback, same encoding size, not inside an IT block, cache
 * off in functions with volatile accesses).  Knob: TCC_DISABLE_PASS=codegen:str_elide. */
int strldr_cache_str_is_redundant(int rt, int rn, int imm, uint32_t puw, int size)
{
  if (strldr_str_elide_off || !thumb_gen_state.generating_function || puw != 6 || size == 0)
    return 0;
  return strldr_cache_try_match_ldr(rt, rn, imm, puw, size, 4);
}

/* Both halves of an LDRD already sitting in the registers it would load them
 * into: the instruction moves nothing.  Asked per half, so a pair written by
 * two single STRs counts as much as one written by a STRD.  Neither register
 * may be the base -- the load would otherwise overwrite the address it reads
 * from, and the entries describe a slot that no longer exists. */
int strldr_cache_ldrd_is_redundant(int rt, int rt2, int rn, int imm, uint32_t puw)
{
  if (!thumb_gen_state.generating_function || puw != 6)
    return 0;
  if (rt == rn || rt2 == rn)
    return 0;
  return strldr_cache_try_match_ldr(rt, rn, imm, puw, 4, 4) &&
         strldr_cache_try_match_ldr(rt2, rn, imm + 4, puw, 4, 4);
}

/* Decode T1/T2/T3 STR/LDR immediate-offset forms with no writeback.
 * Returns 1 and fills outputs when the opcode matches, 0 otherwise.
 * *is_str_out is 1 for STR, 0 for LDR. */
int decode_str_ldr_imm(thumb_opcode op, int *is_str_out, int *rt_out, int *rn_out, int *imm_out,
                              uint32_t *puw_out, int *width_out)
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
      *width_out = 4;
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
      *width_out = 4;
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
      *width_out = 1;
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
      *width_out = 2;
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
      *width_out = 1 << ((hi >> 5) & 3); /* size field: 0=byte, 1=half, 2=word */
      return 1;
    }
    /* T4: STR/LDR indexed forms (imm8, PUW at lo[10:8], marker lo[11]=1):
     * 0xF84x lo[11]=1 is STR post/pre-indexed or negative-offset, 0xF85x the
     * LDR forms, 0xF80x/F81x/F82x/F83x the byte/half ones, 0xF91x/F93x the
     * signed loads.  hi bit 7 clear separates these from every imm12 form
     * (0xF88x..0xF8Dx, 0xF99x/0xF9Bx), whose low 12 bits could otherwise
     * fake the lo[11] marker.  These are the write-back encodings
     * ra:load_postinc / ra:store_postinc emit: on W=1 the BASE register
     * changes, which the caller must see or every cache keyed on Rn goes
     * stale (the postinc fusion's whole point is that Rn no longer holds the
     * old address). */
    if ((hi & 0xFE80) == 0xF800 && (lo & 0x0800))
    {
      int is_ldr = (hi >> 4) & 1;
      int rn = hi & 0xF;
      if (rn == 0xF)
        return 0; /* PC-relative literal form (imm12 with bit 11 set); skip. */
      *is_str_out = !is_ldr;
      *rn_out = rn;
      *rt_out = (lo >> 12) & 0xF;
      *imm_out = lo & 0xFF;
      *puw_out = (lo >> 8) & 0x7;
      *width_out = 1 << ((hi >> 5) & 3); /* size field: 0=byte, 1=half, 2=word */
      return 1;
    }
    return 0;
  }
  return 0;
}

/* LDRD/STRD (Thumb-2), immediate offset: 1110 100P U1W0 nnnn (STRD) /
 * 1110 100P U1W1 nnnn (LDRD), imm8 word-scaled in the low halfword.  These
 * move EIGHT bytes, so a cache that only understands 4-byte accesses has to
 * see them or a `strd` would silently leave a stale entry for either half. */
int decode_strd_ldrd_imm(thumb_opcode op, int *is_str_out, int *rn_out, int *imm_out)
{
  if (op.size != 4)
    return 0;
  uint16_t hi = (uint16_t)((op.opcode >> 16) & 0xFFFF);
  if ((hi & 0xFE40) != 0xE840)
    return 0;
  int rn = hi & 0xF;
  if (rn == 0xF)
    return 0; /* PC-relative literal form. */
  int add = (hi >> 7) & 1;
  int imm = (int)((op.opcode & 0xFF) << 2);
  *is_str_out = !((hi >> 4) & 1);
  *rn_out = rn;
  *imm_out = add ? imm : -imm;
  return 1;
}

/* Emit LDR Rt, [Rn, #imm] unless the STR-cache already knows Rt still
 * holds [Rn+imm] from an unclobbered earlier STR, in which case emission
 * is skipped entirely.  ot() is never called in the elided path, so `ind`
 * and code_size only advance for real emissions — same contract as the
 * MOV coalescing helper. */
int ot_check_ldr_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc)
{
  thumb_opcode ins = th_ldr_imm(rt, rn, imm, puw, enc);
  if (thumb_gen_state.generating_function && puw == 6 && ins.size != 0 &&
      strldr_cache_try_match_ldr((int)rt, (int)rn, imm, puw, ins.size, 4))
  {
    /* Redundant reload: Rt still holds [Rn+imm] from an earlier STR that
     * has not been clobbered.  No emission, no cache update needed — the
     * existing entry remains accurate. */
    return 0;
  }
  return ot_check(ins);
}

/* Emit STR Rt, [Rn, #imm], unless the frame-slot cache knows Rt already
 * holds [Rn+imm] (strldr_cache_str_is_redundant).  The cache-record side
 * effect of an emitted store happens inside ot() once the opcode is
 * classified. */
int ot_check_str_imm(uint32_t rt, uint32_t rn, int imm, uint32_t puw, thumb_enforce_encoding enc)
{
  thumb_opcode ins = th_str_imm(rt, rn, imm, puw, enc);
  if (strldr_cache_str_is_redundant((int)rt, (int)rn, imm, puw, ins.size))
    return 1;
  return ot_check(ins);
}
uint32_t mapcc(int cc)
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

static void th_literal_pool_init()
{
  thumb_gen_state.literal_pool_size = 64;
  thumb_gen_state.literal_pool_count = 0;
  thumb_gen_state.pool_window_first = -1;
  thumb_gen_state.pool_bytes = 0;
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
  case ARM_FPU_RP2350:
    return "rp2350";
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

  /* -mfloat-abi=soft means "emit no FP instructions at all", whatever -mfpu
   * names.  Resolving that to the soft table here -- rather than only checking
   * the ABI at the emitters -- keeps the one contract the has_* bits carry:
   * ir_put_soft_call_fpu_if_needed() reads them to decide whether the op stays
   * an __aeabi_ call, and ir_op_is_implicit_call_ra() reads the SAME bits to
   * decide whether it still clobbers r0-r3.  With the bits set but the emitter
   * refusing to inline, the backend emits a BL whose clobber the allocator no
   * longer models: wrong code, not a missed optimisation.
   *
   * Reachable in practice since a build can ship a default -mfpu
   * (CONFIG_TCC_DEFAULT_FPU): before that, `-mfloat-abi=soft` alone left
   * fpu_type at AUTO and landed on the soft table by the check above. */
  if (s->float_abi == ARM_SOFT_FLOAT)
  {
    return &arm_soft_fpu_config;
  }

  /* -mfp-inline=none: keep the FPU the target has -- so the link still selects
   * the hardware-backed runtime and the image still declares it needs the unit
   * -- but route every operation through a call.  Returning the soft table is
   * how that is spelled, for the same reason -mfloat-abi=soft does above: the
   * has_* bits are read twice, once by ir_put_soft_call_fpu_if_needed() to
   * decide whether the op stays an __aeabi_ call and once by
   * ir_op_is_implicit_call_ra() to decide whether it still clobbers r0-r3.
   * Refusing to inline anywhere but here would leave the second reader
   * believing an operation that is now a BL keeps its registers. */
  if (s->fp_inline == ARM_FP_INLINE_NONE)
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
  case ARM_FPU_RP2350:
    return &arm_rp2350_dcp_fpu_config;
  default:
    fprintf(stderr, "unsupported FPU type: %d for ARM architecture", s->fpu_type);
    exit(1);
    return NULL;
  }
}

/* Report the ARM EABI build attributes for the code this invocation emits.
 * Values follow the ABI addenda; readelf -A renders them.  The FP trio is what
 * makes an object's float ABI checkable by a linker:
 *   Tag_FP_arch       — which FP unit's instructions may appear (0 = none)
 *   Tag_ABI_HardFP_use — 1 ("SP only") when the unit has no double precision
 *   Tag_ABI_VFP_args  — 1 only for -mfloat-abi=hard; its absence means the base
 *                       (GPR) argument standard, which is what soft/softfp use. */
ST_FUNC void arm_get_eabi_attrs(struct TCCState *s, ArmEabiAttrs *out)
{
  /* Thumb-2 is the mainline/baseline discriminator: v8-M.baseline has no t32. */
  const int mainline = arm_target_dependent.feat.t32 ? 1 : 0;
  out->cpu_name = mainline ? "8-M.MAIN" : "8-M.BASE";
  out->cpu_arch = mainline ? 17 /* v8-M.mainline */ : 16 /* v8-M.baseline */;

  out->fp_arch = 0;
  out->hardfp_use = 0;
  out->vfp_args = 0;

  /* -mfloat-abi=soft emits no FP instructions at all, so it advertises no FP
   * unit even when -mfpu names one. */
  if (s->float_abi == ARM_SOFT_FLOAT)
    return;

  switch (s->fpu_type)
  {
  case ARM_FPU_FPV5_SP_D16:
  case ARM_FPU_RP2350:
    out->fp_arch = 8;    /* FPv5/FP-D16 for ARMv8 */
    out->hardfp_use = 1; /* single precision only */
    break;
  case ARM_FPU_FPV5_D16:
    out->fp_arch = 8; /* same unit, double precision present */
    break;
  case ARM_FPU_FPV4_SP_D16:
    out->fp_arch = 6; /* VFPv4-D16 */
    out->hardfp_use = 1;
    break;
  case ARM_FPU_NONE:
    return;
  default:
    /* AUTO and the non-M-profile units: describe what the resolved unit can
     * do rather than guessing a name. */
    out->fp_arch = 8;
    if (architecture_config.fpu && !architecture_config.fpu->has_dadd)
      out->hardfp_use = 1;
    break;
  }

  if (s->float_abi == ARM_HARD_FLOAT)
    out->vfp_args = 1; /* FP arguments/results in VFP registers */
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
  sb_relative_got = s->sb_relative_got;
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
    /* Hold s14/s15 out of the allocator: the single-precision FP emitters use
     * them as fixed VFP scratch (VFP_SCRATCH0/1) to shuttle GPR/imm/spill
     * operands through the FPU without clobbering a live float. */
    s->float_registers_map_for_allocator &= ~((1ull << VFP_SCRATCH0) | (1ull << VFP_SCRATCH1));
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
  thumb_gen_state.pool_window_first = -1;
  thumb_gen_state.pool_bytes = 0;
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
void th_literal_pool_generate(void)
{
  /* Not between the virtual instructions of an outlined window: the real pass
   * is shorter than the rehearsal, which placed no pool there. */
  if (tcc_gen_machine_outline_suppressing())
    return;
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
  pool_flushes_total++;
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
  small_sequence(ThumbLitPosSeq) literal_positions_owner = {0};
  ThumbLitPosSeq_init(&literal_positions_owner, (size_t)pool_count);
  int *literal_positions = ThumbLitPosSeq_data(&literal_positions_owner);

  map_sym_d();

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
  /* A dry run advances `ind` without growing the section, so branch_pos can
   * point past cur_text_section->data — patching there corrupts the heap.  The
   * bytes are discarded anyway.  (Valgrind: invalid write of size 2.) */
  if (thumb_gen_state.generating_function && !dry_run_state.active)
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
  map_sym_t();

  /* Second pass: patch all instructions to point to correct literal position */
  for (int i = 0; i < pool_count; i++)
  {
    ThumbLiteralPoolEntry *entry = &pool[i];
    int literal_pos = literal_positions[i];
    int aligned_position = ((literal_pos - entry->patch_position) + 3) & ~3;

    /* Same reason as the skip-branch above: during a dry run `ind` has run past
     * the end of cur_text_section->data, so neither the diagnostic reads below
     * nor the patches may touch it. */
    if (dry_run_state.active)
      continue;

    uint16_t b0_prev = 0, b1_prev = 0;
    if (thumb_gen_state.generating_function)
    {
      b0_prev = *(uint16_t *)(cur_text_section->data + branch_pos);
      b1_prev = *(uint16_t *)(cur_text_section->data + branch_pos + 2);
    }

    /* The offset MUST fit the encoding.  Masking it into place unchecked --
     * which is what these three sites used to do -- turns an out-of-range pool
     * into a load from whatever code happens to sit at (pc + offset mod range):
     * no diagnostic, a plausible-looking binary, and a fault or silent garbage
     * only when that path executes.  th_literal_pool_would_flush_for() is
     * supposed to keep the distance in range, but its budget is an estimate
     * (see the note on tcc_gen_machine_cbz_forward_ok) and it has drifted
     * before, so this is the backstop that makes such a drift a build failure
     * instead of a miscompile. */
    int field = aligned_position - 4;
    int limit = (entry->short_instruction || entry->data_size == 8) ? (0xff << 2) : 0xfff;
    if (field < 0 || field > limit)
      tcc_error("compiler_error: literal pool out of range for %s at 0x%x: offset %d exceeds %d "
                "(pool at 0x%x). The flush budget in ot() let the pool drift too far from the load.",
                entry->short_instruction ? "LDR(T1)" : (entry->data_size == 8 ? "LDRD" : "LDR.W"),
                entry->patch_position, field, limit, literal_pos);

    // patch the instruction that references this literal
    if (entry->short_instruction)
    {
      /* Short LDR literal (T1): imm8 word-aligned in bits 0-7 */
      uint16_t *patch_ins = (uint16_t *)(cur_text_section->data + entry->patch_position);
      *patch_ins |= ((field >> 2) & 0x00ff);
    }
    else if (entry->data_size == 8)
    {
      /* LDRD literal: imm8 word-aligned in bits 0-7 of second halfword, P=1 U=1 in first halfword */
      uint16_t *patch_ins0 = (uint16_t *)(cur_text_section->data + entry->patch_position);
      uint16_t *patch_ins1 = (uint16_t *)(cur_text_section->data + entry->patch_position + 2);
      /* Set P=1 (bit 8) and U=1 (bit 7) for positive offset, pre-indexed */
      *patch_ins0 |= (1 << 8) | (1 << 7); /* P and U bits */
      *patch_ins1 |= ((field >> 2) & 0x00ff);
    }
    else
    {
      /* Long LDR literal (T2): imm12 byte offset in bits 0-11 of second halfword */
      uint16_t *patch_ins = (uint16_t *)(cur_text_section->data + entry->patch_position + 2);
      *patch_ins |= (field & 0x0fff);
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

  thumb_gen_state.literal_pool_count = 0;
  thumb_gen_state.pool_window_first = -1;
  thumb_gen_state.pool_bytes = 0;
  thumb_gen_state.code_size = 0;
  generating_pool = 0;
  /* Clear the hash table after flushing pool */
  literal_pool_hash_clear(&literal_pool_hash);
  literal_pool_lookup_cache_clear(&literal_pool_last_lookup);
}

/* Exact span from the earliest pending literal load to the projected end of
 * the pool if it were flushed after `upcoming_bytes` more code: measured from
 * `ind` (ground truth — includes raw o() emissions such as switch_to_data
 * jump tables that bypass code_size accounting; verbs.c/zork drifted 236
 * bytes that way and blew the old `code_size + pool_count * 4` proxy), plus
 * the B.W over the pool, worst-case alignment pad, and the exact pool bytes.
 * Runs identically in the dry and real passes: both advance `ind` and both
 * update the window fields through th_literal_pool_note_entry(). */
int th_pool_span_after(int upcoming_bytes)
{
  int pool_count = dry_run_state.active ? dry_run_literal_pool_count : thumb_gen_state.literal_pool_count;
  if (pool_count == 0 || thumb_gen_state.pool_window_first < 0)
    return 0;
  return (ind - thumb_gen_state.pool_window_first) + upcoming_bytes + 4 /* B.W */ + 2 /* align */ +
         thumb_gen_state.pool_bytes;
}

/* Fold a freshly initialized entry into the window bookkeeping.  Must run
 * after the caller has set patch_position/data_size (find_or_allocate cannot:
 * those fields are filled in afterwards). */
void th_literal_pool_note_entry(const ThumbLiteralPoolEntry *entry)
{
  if (thumb_gen_state.pool_window_first < 0 || entry->patch_position < thumb_gen_state.pool_window_first)
    thumb_gen_state.pool_window_first = entry->patch_position;
  if (entry->shared_index == -1)
    thumb_gen_state.pool_bytes += (entry->data_size == 8) ? 8 : 4;
}
void th_literal_pool_reserve_upcoming_bytes(int upcoming_bytes)
{
  if (!thumb_gen_state.generating_function)
    return;

  if (th_pool_span_after(upcoming_bytes) >= THUMB_POOL_RANGE_LIMIT - THUMB_POOL_FLUSH_SLACK)
    th_literal_pool_generate();
}
int th_literal_pool_would_flush_for(int upcoming_bytes)
{
  if (!thumb_gen_state.generating_function)
    return 0;

  return th_pool_span_after(upcoming_bytes) >= THUMB_POOL_RANGE_LIMIT - THUMB_POOL_FLUSH_SLACK;
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
/* B / B<c> / CBZ / CBNZ: transfer control and write nothing else.  BL, BLX and
 * BX are deliberately NOT here -- BL/BLX write LR and clobber caller-saved
 * registers and memory, and BX ends the block. */
int thumb_op_is_plain_branch(thumb_opcode op)
{
  if (op.size == 2)
  {
    uint16_t hw = (uint16_t)(op.opcode & 0xFFFF);
    /* B<c> T1: 1101 cccc imm8.  cond 1110 is UDF and 1111 is SVC. */
    if ((hw & 0xF000) == 0xD000)
    {
      int cond = (hw >> 8) & 0xF;
      return cond != 0xE && cond != 0xF;
    }
    /* B T2: 11100 imm11. */
    if ((hw & 0xF800) == 0xE000)
      return 1;
    /* CBZ/CBNZ: 1011 op 0 i 1 imm5 Rn3. */
    if ((hw & 0xF500) == 0xB100)
      return 1;
    return 0;
  }
  if (op.size == 4)
  {
    uint16_t hi = (uint16_t)((op.opcode >> 16) & 0xFFFF);
    uint16_t lo = (uint16_t)(op.opcode & 0xFFFF);
    if ((hi & 0xF800) != 0xF000)
      return 0;
    /* lo[15:14,12]: 10x0 = B T3 (conditional), 10x1 = B T4.  BL is 11x1 and
     * BLX is 11x0, both of which write LR. */
    if ((lo & 0xD000) == 0x8000 || (lo & 0xD000) == 0x9000)
      return 1;
    return 0;
  }
  return 0;
}
int thumb_op_is_pure_flag_setter(thumb_opcode op)
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
int thumb_decode_dest_reg(thumb_opcode op)
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
