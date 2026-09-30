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

/* Thumb-2 code generator: return values, prologue and epilogue, LEA, stack
 * stores and direct/indirect call emission. */

#include "arm-thumb-gen.h"

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

  /* Hard-float single-precision return: the value goes in s0 (AAPCS VFP). */
  if (tcc_state && tcc_state->float_abi == ARM_HARD_FLOAT && !src.is_64bit && src.btype == IROP_BTYPE_FLOAT32)
  {
    if (src.kind == MACH_OP_VFP_REG)
    {
      if (src.u.reg.r0 != 0)
        ot_check(th_vmov_register(0, (uint16_t)src.u.reg.r0, 0)); /* s0 = s_src */
    }
    else
    {
      MachineCodegenContext ctx = {0};
      int r = mach_ensure_in_reg(&ctx, &src, 0);
      ot_check(th_vmov_gp_sp((uint16_t)r, 0, 0)); /* s0 = r */
      mach_release_all(&ctx);
    }
    return;
  }

  /* Hard-float double return: the value goes in d0 (AAPCS VFP).  Doubles are
   * not VFP-resident here, so materialize the GPR pair and pack it into d0. */
  if (tcc_state && tcc_state->float_abi == ARM_HARD_FLOAT && src.is_64bit && src.btype == IROP_BTYPE_FLOAT64 &&
      !src.is_complex)
  {
    MachineCodegenContext ctx = {0};
    MachineOperand lo = mach_make_lo_half(&src);
    MachineOperand hi = mach_make_hi_half(&src);
    lo.btype = IROP_BTYPE_INT32;
    hi.btype = IROP_BTYPE_INT32;
    int rlo = mach_ensure_in_reg(&ctx, &lo, 0);
    int rhi = mach_ensure_in_reg(&ctx, &hi, (1u << (uint32_t)rlo));
    ot_check(th_vmov_2gp_dp((uint16_t)rlo, (uint16_t)rhi, 0, 0)); /* d0 = rlo,rhi */
    mach_release_all(&ctx);
    return;
  }

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

/* Does some call of the function reload R9 from its save slot afterwards --
 * the only reader of the slot the prologue stores it to: one through a
 * pointer, a __builtin_apply, or a direct call to a callee not known to be in
 * this module (restore_r9 at the call site). */
ST_FUNC int tcc_gen_machine_calls_reload_r9(TCCIRState *ir)
{
  for (int i = 0; i < ir->next_instruction_index; i++)
  {
    IRQuadCompact *q = &ir->compact_instructions[i];
    if (q->op == TCCIR_OP_BUILTIN_APPLY)
      return 1;
    if (q->op != TCCIR_OP_FUNCCALLVAL && q->op != TCCIR_OP_FUNCCALLVOID)
      continue;
    IROperand fn = tcc_ir_op_get_src1(ir, q);
    MachineOperand m = machine_op_from_ir(ir, &fn);
    if (!thumb_callee_in_this_module(&m) && !thumb_callee_noreturn(&m))
      return 1;
  }
  return 0;
}

/* Load the stack-passed parameter at parameter offset `off` into the core
 * register(s) the allocator kept it in.  An offset beyond the immediate forms
 * is built in the destination itself: in the prologue no other register is
 * known to be free, as any may hold a parameter. */
static void prologue_load_stack_param(int lo, int hi, int off)
{
  const int base = tcc_state->need_frame_pointer ? R_FP : R_SP;
  const int adj = param_frame_offset(off);
  if (hi < 0)
  {
    if (load_word_from_base(lo, base, adj, 0))
      return;
    if (!ot(th_generic_mov_imm(lo, adj)))
      load_full_const(lo, PREG_NONE, LFC_SPLIT(adj));
    ot_check(th_ldr_reg(lo, base, lo, THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
    return;
  }
  if (adj + 4 <= 4095)
  {
    load_from_base(lo, hi, IROP_BTYPE_INT64, 0, adj, 0, base);
    return;
  }
  if (!ot(th_generic_mov_imm(lo, adj)))
    load_full_const(lo, PREG_NONE, LFC_SPLIT(adj));
  ot_check(th_add_reg(lo, base, lo, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE));
  ot_check(th_ldr_imm(hi, lo, 4, 6, ENFORCE_ENCODING_NONE));
  ot_check(th_ldr_imm(lo, lo, 0, 6, ENFORCE_ENCODING_NONE));
}

/* Whether the last prologue saved LR: the body may then BL. */
static int prolog_saved_lr;
ST_FUNC int tcc_gen_machine_prolog_saved_lr(void)
{
  return prolog_saved_lr;
}

/* -Os outliner: a window the real pass calls instead begins.  Nothing pairs
 * across its edges: the rehearsal would have put such a pair's halves in two
 * ops, and neither can be in a window. */
ST_FUNC void tcc_gen_machine_outline_window_open(void)
{
  frame_word_last.kind = 0;
}

/* The window's ops are done, and `bytes` of them were recorded, not emitted:
 * the BL to the shared body replaces them. */
ST_FUNC void tcc_gen_machine_outline_window_closed(int bytes)
{
  thumb_gen_state.code_size -= bytes;
  frame_word_last.kind = 0;
}

/* The BL to a shared body.  Not through ot(): the caches saw the window's own
 * instructions, which is what the body runs, not a call.  Same pool check as
 * ot(). */
ST_FUNC void tcc_gen_machine_outline_emit_bl(int esym)
{
  thumb_gen_state.code_size += 4;
  if (th_pool_span_after(4) >= THUMB_POOL_RANGE_LIMIT - THUMB_POOL_FLUSH_SLACK)
    th_literal_pool_generate();
  int pos = ind;
  thumb_opcode bl = th_bl_t1((uint32_t)-4);
  o(bl.opcode >> 16);
  o(bl.opcode & 0xffff);
  put_elf_reloc(symtab_section, cur_text_section, pos, R_ARM_THM_JUMP24, esym);
}

/* One halfword of a window put back where it was generated (already counted
 * in code_size when ot() recorded it). */
ST_FUNC void tcc_gen_machine_outline_emit_raw(uint16_t hw)
{
  o(hw);
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
  pool_flush_it_pending = 0;
  TCCIRState *ir = tcc_state->ir;

  /* Determine if LR needs saving */
  int save_lr = !leaffunc || tcc_state->force_lr_save;
  if (extra_prologue_regs & (1u << R_LR))
    save_lr = 1;
  prolog_saved_lr = save_lr;

  /* Variadic functions need a stable FP for va_list setup. */
  if (func_var)
    tcc_state->need_frame_pointer = 1;

  const int need_fp = (tcc_state->force_frame_pointer || tcc_state->need_frame_pointer);
  tcc_state->need_frame_pointer = need_fp;

  /* The allocator hands out R7 only when ra_may_need_frame_pointer predicted
   * no frame pointer; a function that reaches here needing FP with R7
   * allocated means the prediction missed a forcing condition, and silently
   * continuing would use one register as frame base and value at once. */
  if (need_fp && (used_registers & (1ULL << R_FP)))
    tcc_error("compiler_error: R7 allocated in a frame-pointer function "
              "(ra_may_need_frame_pointer out of sync with a forcing site)");

  /* Collect callee-saved registers */
  uint16_t callee_regs_local = 0;
  int callee_count = 0;
  for (int i = R4; i <= R11; ++i)
  {
    if (tcc_state->text_and_data_separation && i == R9)
      continue;
    if (i == R_FP && need_fp)
      continue; /* r7 is the frame base; pushed below with LR */
    if (used_registers & (1ULL << i))
    {
      callee_regs_local |= (1 << i);
      callee_count++;
    }
  }

  /* Registers codegen asks to be saved on top of the allocation: the static
   * chain register (R10) for nested functions, and the rodata anchor. */
  for (int i = R4; i <= R11; ++i)
  {
    if (!(extra_prologue_regs & (1u << i)))
      continue;
    if (tcc_state->text_and_data_separation && i == R9)
      continue;
    if (i == R_FP && need_fp)
      continue;
    if (callee_regs_local & (1u << i))
      continue;
    callee_regs_local |= (1u << i);
    callee_count++;
  }

  int push_align_pad = 0; /* 4 if push count is odd, absorbed into SUB SP */

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
   * When there are no locals (stack_size == 0), pad by pushing a dummy low
   * register (R3) — avoids SUB SP + ADD SP, saving 2 insns.  When locals
   * exist, absorb the gap into SUB SP instead, keeping PUSH in 16-bit
   * encoding (no high regs like R12). */
  if (registers_count % 2 != 0)
  {
    if (stack_size == 0)
    {
      registers_to_push |= (1 << R3);
      registers_count++;
    }
    else
    {
      push_align_pad = 4;
    }
  }

  map_sym_t();

  /* Variadic, or a parameter straddling r3 and the stack: push r0-r3 FIRST
   * so they are contiguous with the stack args. */
  vararg_push_size = 0;
  if (func_var || (ir && ir->push_arg_regs))
  {
    ot_check(th_push((1 << R0) | (1 << R1) | (1 << R2) | (1 << R3)));
    vararg_push_size = 16;
  }

  offset_to_args = registers_count * 4 + (func_var ? vararg_push_size : 0);

  if (registers_count > 0)
    ot_check(th_push(registers_to_push));

  pushed_registers = registers_to_push;

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
  if (stack_size > 0)
  {
    gadd_sp(-stack_size);
  }

  /* The frame pointer is the SP left here, the bottom of the frame: locals
   * and incoming parameters sit at the same positive offsets from it as from
   * SP (see fp_adjust_local_offset).  SP moves only for VLA/alloca, and for
   * the soft-float helpers' and scratch pushes' balanced windows. */
  if (need_fp)
  {
    if (!ot(th_add_imm(R_FP, R_SP, 0, flags_safe(), ENFORCE_ENCODING_NONE)))
    {
      fprintf(stderr, "compiler_error: prolog frame pointer setup failed\n");
      exit(1);
    }
  }

  /* SP (and the frame pointer) is lower by the full SUB SP amount (locals +
   * alignment pad): incoming stack parameters are that much further up.
   * Local addressing uses allocated_stack_size (without pad): the pad sits at
   * the top of the SUB SP region, right below the pushed registers, so
   * locals occupy base+0 .. base+allocated_stack_size-1. */
  offset_to_args += epilogue_stack_dealloc;

  /* Everything from here to the parameter shuffle below runs before a single
   * incoming value has been moved to its allocated home, so these registers
   * are all still live and none of them may be taken as a scratch register:
   * R0-R3 (the argument registers -- all four, not just the named ones, since
   * a variadic function saves the anonymous ones too), R9 (the GOT base under
   * text_and_data_separation) and the static chain register.
   *
   * A store whose offset does not fit the STR immediate needs a register to
   * hold the offset, and the scratch picker's idea of "free" comes from the
   * live intervals, which do not model the incoming registers at prologue
   * position.  It therefore handed out R0.  In a function with alloca and a
   * frame deeper than the 255-byte negative STR immediate -- glob() and
   * fnmatch() in GNU make's gnulib, among others -- the R9 spill below then
   * emitted
   *     movw r0,#0x131c ; rsb r0,r0,#0 ; str.w r9,[r7,r0]
   *     movw r4,#0x1018 ; rsb r4,r4,#0 ; str   r0,[r7,r4]
   * and the first parameter, which the second store was meant to spill, was
   * the offset constant by the time it got there. */
  uint32_t prologue_incoming_mask = (1u << R0) | (1u << R1) | (1u << R2) | (1u << R3);
  if (tcc_state->text_and_data_separation)
    prologue_incoming_mask |= (1u << ARM_R9);
  if (ir && ir->has_static_chain)
    prologue_incoming_mask |= (1u << architecture_config.static_chain_reg);

  /* Save the PIC GOT base (R9) once, into slot 0 of the nested-call save area.
   *
   * R9 is caller-saved under text_and_data_separation, so every call reloads
   * it from the frame; but the value never changes within a function, so the
   * store only has to happen once.  It used to be emitted at each call site
   * next to the reload, which cost 45,329 instructions (165 KiB) in the
   * compiler's own build and produced runs of literally
   *   ldr.w r9, [sp] ; str.w r9, [sp]
   * where the reload was immediately followed by writing the same value back.
   *
   * call_nested_save_size is non-zero exactly when the function has at least
   * one call under text_and_data_separation (ir/codegen.c, where the area is
   * sized), so it is the same guard in both codegen passes — and R9 is saved
   * at every one of those call sites, so slot 0 is always reserved for it. */
  if (tcc_state->text_and_data_separation && ir && ir->call_nested_save_size > 0 && tcc_gen_machine_calls_reload_r9(ir))
  {
    const int r9_slot_offset = ir->call_outgoing_size;
    /* SP-relative, like the store_word_to_stack() of the call sites.  A
     * VLA/alloca function addresses the slots off the frame pointer instead,
     * at the same offset: here, before any dynamic allocation, both are the
     * bottom of the frame. */
    tcc_gen_machine_store_to_sp_ex(ARM_R9, r9_slot_offset, prologue_incoming_mask);
  }

  /* Save incoming static chain (R10) at its fixed slot, frame offset -4
   * (CHAIN_SLOT_OFFSET), where the body and tcc_gen_machine_restore_chain
   * read it back. */
  if (ir && ir->has_static_chain)
  {
    tcc_gen_machine_store_to_stack_ex(architecture_config.static_chain_reg, fp_adjust_local_offset(-4, 0),
                                      prologue_incoming_mask);
  }

  /* For variadic functions, save incoming r0-r3 in a fixed area at frame
   * offsets -16..-4 (for named parameter access).
   * The PUSH {r0-r3} at function entry already creates a contiguous register
   * save area above the callee-saved pushes, adjacent to the stack arguments.
   * __gr_top points to the end of that area (= start of stack args).
   */
  if (func_var)
  {
    /* Store r0-r3 at frame offsets -16..-4 for named parameter access.
     * (The contiguous PUSH'd copy is at FP+offset_to_args-16..FP+offset_to_args-4
     * and is used by va_arg for anonymous argument traversal.) */
    tcc_gen_machine_store_to_stack_ex(R0, fp_adjust_local_offset(-16, 0), prologue_incoming_mask);
    tcc_gen_machine_store_to_stack_ex(R1, fp_adjust_local_offset(-12, 0), prologue_incoming_mask);
    tcc_gen_machine_store_to_stack_ex(R2, fp_adjust_local_offset(-8, 0), prologue_incoming_mask);
    tcc_gen_machine_store_to_stack_ex(R3, fp_adjust_local_offset(-4, 0), prologue_incoming_mask);

    /* The frame-metadata triple that used to live at FP-20/-24/-28 (__gr_top,
     * named_arg_reg_bytes, named_arg_stack_bytes) is gone: it existed purely so
     * the runtime __tcc_va_start could rediscover the address of the first
     * anonymous argument.  TOK_builtin_va_start now materializes that address
     * directly (a PARAM-relative LEA), so those six prologue instructions were
     * dead in every variadic function. */
  }

  /* __builtin_apply_args: save incoming r0-r3 and stack args pointer
   * to the reserved apply_args block so __builtin_apply can replay them.
   * Layout at apply_args_offset: [stack_args_ptr, r0, r1, r2, r3]. */
  if (tcc_state->func_save_apply_args && ir)
  {
    int adj = fp_adjust_local_offset(tcc_state->apply_args_offset, 0);

    /* Store stack args pointer (FP + offset_to_args = start of stack args area) */
    {
      const int fp_or_sp = tcc_state->need_frame_pointer ? R_FP : R_SP;
      ot_check(th_add_imm(R_IP, fp_or_sp, offset_to_args, flags_safe(), ENFORCE_ENCODING_NONE));
    }
    tcc_gen_machine_store_to_stack_ex(R_IP, adj, prologue_incoming_mask);

    /* Store r0-r3 at offsets +4, +8, +12, +16 from the block start */
    tcc_gen_machine_store_to_stack_ex(R0, adj + 4, prologue_incoming_mask);
    tcc_gen_machine_store_to_stack_ex(R1, adj + 8, prologue_incoming_mask);
    tcc_gen_machine_store_to_stack_ex(R2, adj + 12, prologue_incoming_mask);
    tcc_gen_machine_store_to_stack_ex(R3, adj + 16, prologue_incoming_mask);
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

    /* Stack-passed parameters the allocator kept in core registers
     * (tcc_ir_avoid_spilling_stack_passed_params).  Loaded last: a load may
     * target a register an incoming argument still occupies until step 2. */
    typedef struct StackParamLoad
    {
      int lo, hi, off;
    } StackParamLoad;
    StackParamLoad *stack_loads = tcc_malloc(sizeof(StackParamLoad) * (ir->next_parameter + 1));
    int stack_load_count = 0;

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

    /* Hard-float double parameters arrive in d0-d7 but are not VFP-resident, so
     * each is unpacked into its allocated GPR pair.  Collected here and emitted
     * after the GPR parallel move: the unpack overwrites GPRs that may still
     * hold incoming GPR arguments. */
    typedef struct DoubleUnpack
    {
      int dreg, lo, hi;
    } DoubleUnpack;
    DoubleUnpack dbl_unpack[8];
    int dbl_unpack_count = 0;

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
        if (interval->allocation.offset == 0 && alloc_r0 <= R12)
        {
          stack_loads[stack_load_count++] =
              (StackParamLoad){.lo = alloc_r0, .hi = is_64bit ? alloc_r1 : -1, .off = interval->original_offset};
          continue;
        }
        /* Captured by a nested function: the caller's argument area is not a
         * home the child can name, so this parameter was given a slot in OUR
         * frame.  Copy the incoming value into it — the child reads the slot
         * chain-relative, and every parent access goes through it too, which
         * is what keeps the by-reference semantics. */
        if (interval->nested_home && interval->allocation.offset != 0)
        {
          const int dst = fp_adjust_local_offset(interval->allocation.offset, 0);
          ScratchRegAlloc sc = get_scratch_reg_with_save(incoming_arg_regs_mask);
          prologue_load_stack_param(sc.reg, -1, interval->original_offset);
          tcc_gen_machine_store_to_stack_ex(sc.reg, dst, incoming_arg_regs_mask);
          if (is_64bit)
          {
            prologue_load_stack_param(sc.reg, -1, interval->original_offset + 4);
            tcc_gen_machine_store_to_stack_ex(sc.reg, dst + 4, incoming_arg_regs_mask);
          }
          restore_scratch_reg(&sc);
          continue;
        }
        /* Otherwise it lives in the caller's argument area.  Leave the
         * allocation empty so IR materialization treats it as a VT_PARAM
         * lvalue and loads it where it is used. */
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
        /* Float/double param arriving in a VFP register but spilled to the
         * stack: bridge it through GPR scratch to reuse the ordinary store.  A
         * double occupies d<n> and needs both halves written. */
        if (is_vfp_reg(incoming_r0))
        {
          ScratchRegAlloc sc = get_scratch_reg_with_save(incoming_arg_regs_mask);
          if (is_64bit)
          {
            ScratchRegAlloc sc2 = get_scratch_reg_with_save(incoming_arg_regs_mask | (1u << (uint32_t)sc.reg));
            ot_check(th_vmov_2gp_dp((uint16_t)sc.reg, (uint16_t)sc2.reg,
                                    (uint16_t)(vfp_num(incoming_r0) / 2), 1)); /* lo,hi = d<n> */
            tcc_gen_machine_store_to_stack_ex(sc.reg, stack_offset, incoming_arg_regs_mask);
            tcc_gen_machine_store_to_stack_ex(sc2.reg, stack_offset + 4, incoming_arg_regs_mask);
            restore_scratch_reg(&sc2);
          }
          else
          {
            ot_check(th_vmov_gp_sp((uint16_t)sc.reg, (uint16_t)vfp_num(incoming_r0), 1)); /* gpr = s_incoming */
            tcc_gen_machine_store_to_stack_ex(sc.reg, stack_offset, incoming_arg_regs_mask);
          }
          restore_scratch_reg(&sc);
          continue;
        }
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

      /* Double parameter arriving in a VFP register (hard-float): it is not
       * VFP-resident — there is no double-precision arithmetic here — so unpack
       * d<n> into the GPR pair the allocator gave it.  Emitted before the GPR
       * parallel move, whose sources are all still live. */
      if (is_64bit && is_vfp_reg(incoming_r0) && alloc_r0 != PREG_NONE && alloc_r0 >= 0 && alloc_r1 >= 0)
      {
        /* Deferred: the unpack writes GPRs that may still hold incoming GPR
         * arguments (a leading int parameter sits in r0, and d0 commonly unpacks
         * into r0:r1), so it must run only after the GPR parallel move below has
         * consumed them. */
        if (dbl_unpack_count < (int)(sizeof(dbl_unpack) / sizeof(dbl_unpack[0])))
          dbl_unpack[dbl_unpack_count++] =
              (DoubleUnpack){.dreg = vfp_num(incoming_r0) / 2, .lo = alloc_r0, .hi = alloc_r1};
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
      if (alloc_r0 != PREG_NONE && is_vfp_reg(alloc_r0))
      {
        /* Float parameter homed to a VFP register.  It arrives either in a VFP
         * register (hard-float argument passing) or in a GPR (soft layout);
         * either way move it into its VFP home now, while the incoming register
         * is still live.  VFP destinations never participate in the GPR parallel
         * move (different register file), so emitting here is safe. */
        if (is_vfp_reg(incoming_r0))
        {
          if (incoming_r0 != alloc_r0)
            ot_check(th_vmov_register((uint16_t)vfp_num(alloc_r0), (uint16_t)vfp_num(incoming_r0), 0));
        }
        else
        {
          ot_check(th_vmov_gp_sp((uint16_t)incoming_r0, (uint16_t)vfp_num(alloc_r0), 0));
        }
        continue;
      }
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

    /* Now that every incoming GPR argument has reached its home, it is safe to
     * overwrite the argument registers: unpack the hard-float doubles from
     * d0-d7 into their allocated GPR pairs. */
    for (int i = 0; i < dbl_unpack_count; ++i)
      ot_check(th_vmov_2gp_dp((uint16_t)dbl_unpack[i].lo, (uint16_t)dbl_unpack[i].hi,
                              (uint16_t)dbl_unpack[i].dreg, 1));

    for (int i = 0; i < stack_load_count; ++i)
      prologue_load_stack_param(stack_loads[i].lo, stack_loads[i].hi, stack_loads[i].off);

    tcc_free(stack_loads);
    tcc_free(moves);
  }

  /* Materialise the .rodata base for the body.  Last in the prologue: the
   * parameter shuffle above may borrow any callee-saved register that holds
   * no incoming argument, which includes this one until it is loaded.  R9 is
   * the incoming GOT base and nothing in the prologue writes it. */
  if (rodata_anchor_reg >= 0)
    ot_check_ldr_imm(rodata_anchor_reg, R9, YAFF_RODATA_ANCHOR_GOT_OFFSET, 6, ENFORCE_ENCODING_NONE);
}

/* Return through the saved LR while dropping the r0-r3 pushed below the saved
 * registers (variadic, push_arg_regs): pop the rest, then one post-indexed LDR
 * loads LR's slot into PC and steps SP past it and the argument area.  Two
 * bytes shorter than `pop {.., lr}; add sp; bx lr`, as a 16-bit POP cannot
 * name LR. */
static void epilogue_return_over_pushed_args(uint32_t saved_with_lr)
{
  uint32_t rest = saved_with_lr & ~(1u << R_LR);
  if (rest)
    ot_check(th_pop(rest));
  ot_check(th_ldr_imm(R_PC, R_SP, 4 + vararg_push_size, 3 /* post-indexed, add, writeback */,
                      ENFORCE_ENCODING_32BIT));
}

ST_FUNC void tcc_gen_machine_epilog(int leaffunc)
{
  TRACE("'tcc_gen_machine_epilog'");

  int lr_saved = pushed_registers & (1 << R_LR);

  /* Where SP may have moved at run time -- VLA, alloca, a call to the
   * library alloca() -- restore it from the frame pointer, which is the SP
   * the prologue left; then leave as a frame without one does (the pushed
   * registers include r7). */
  if (tcc_state->need_frame_pointer && (tcc_state->func_dynamic_sp || tcc_state->force_frame_pointer))
    ot_check_mov_reg(R_SP, R_FP, flags_safe(), THUMB_SHIFT_DEFAULT, ENFORCE_ENCODING_NONE, false);
  if (epilogue_stack_dealloc > 0)
    gadd_sp_ex(epilogue_stack_dealloc, R3);
  if (vararg_push_size > 0 && lr_saved)
  {
    /* r0-r3 were pushed below the stack arguments (push_arg_regs). */
    epilogue_return_over_pushed_args(pushed_registers);
  }
  else if (vararg_push_size > 0)
  {
    if (pushed_registers > 0)
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
    ot_check(th_bx_reg(R_LR));
  }

  thumb_gen_state.generating_function = 0;
  /* The anchor register holds this function's .rodata base and the epilogue
   * has just restored the caller's value in it. */
  rodata_anchor_reg = -1;
  th_literal_pool_generate();
  thumb_free_call_sites();
}

ST_FUNC void tcc_gen_machine_finish_noreturn(void)
{
  thumb_gen_state.generating_function = 0;
  rodata_anchor_reg = -1;
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
  tcc_gen_machine_store_to_sp_ex(reg, offset, 0);
}

/* SP-relative store with extra scratch exclusions; see the _ex comment on the
 * FP-relative variant above. */
ST_FUNC void tcc_gen_machine_store_to_sp_ex(int reg, int offset, uint32_t extra_exclude)
{
  int sign = (offset < 0);
  int abs_offset = sign ? -offset : offset;

  if (!store_word_to_base(reg, R_SP, abs_offset, sign))
  {
    ScratchRegAlloc rr_alloc = th_offset_to_reg_ex(abs_offset, sign, (1u << reg) | (1u << R_SP) | extra_exclude);
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
void gcall_or_jump_mop(int is_jmp, MachineOperand target)
{
  /* Tail-call: promote is_jmp so we emit B/BX instead of BL/BLX. */
  if (tail_call_pending)
    is_jmp = 1;

  if (target.kind == MACH_OP_SYMBOL)
  {
    /* Direct call via BL (or B.W for tail call) with relocation. */
    Sym *sym = target.u.sym.sym;
    Sym *helper = thumb_local_libc_helper(sym);
    if (helper)
      sym = helper;
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
        /* No R9 reload follows this call (or this B.W never comes back) on
         * the assumption that the callee is module-local: have the linker
         * check it. */
        if (thumb_callee_needs_local_call_marker(sym))
          greloc(cur_text_section, reloc_sym, call_pos, R_ARM_YASOS_LOCAL_CALL);
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
void load_immediate(int reg, uint32_t imm, Sym *sym, int update_flags)
{
  (void)update_flags; /* Currently not used, reserved for future use */

  /* If there's a symbol, always use literal pool for relocations */
  if (sym)
  {
    _lfc_sym = sym;
    load_full_const(reg, PREG_NONE, imm, 0);
    return;
  }

  /* Plain-constant reuse, same contract as tcc_machine_load_constant: when the
   * per-register materialisation cache still says `reg` holds this exact
   * constant, the MOV is dead.  Call-argument marshalling is where this pays:
   * every stack-passed literal takes a fresh find_call_scratch()+load_immediate
   * pair, and a variadic call with a run of zero arguments therefore emitted
   * `movs rN,#0; str rN,[sp,#k]` once per word instead of hoisting the zero.
   * imm_cache is invalidated by ot()'s emit-level decode and reset at IR
   * boundaries/calls in both the dry and real passes, so the decision (and the
   * resulting code size) stays identical between them. */
  const int cacheable = thumb_gen_state.generating_function && reg >= 0 && reg < 16;
  const int64_t key = (int64_t)(uint32_t)imm;

  if (cacheable && imm_cache[reg].valid && imm_cache[reg].sym == NULL && imm_cache[reg].value == key)
    return;

  /* Try to encode as ARM immediate (supports various rotated 8-bit patterns) */
  if (!ot(th_generic_mov_imm(reg, imm)))
  {
    /* Value doesn't fit in immediate encoding, use literal pool */
    load_full_const(reg, PREG_NONE, imm, 0);
  }

  /* Record after the emit: ot()/load_full_const just invalidated imm_cache[reg]
   * for the instruction they produced. */
  if (cacheable)
  {
    imm_cache[reg].value = key;
    imm_cache[reg].sym = NULL;
    imm_cache[reg].valid = 1;
  }
}
