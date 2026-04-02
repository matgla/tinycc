/*
 *  TCC Unified Logging System
 *
 *  Copyright (c) 2025 Mateusz Stadnik
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

#pragma once

#include <stdio.h>

/* ============================================================================
 * TCC Logging Configuration
 *
 * Each scope can be toggled independently at compile time:
 *   make CFLAGS+='-DTCC_LOG_IR_GEN=1'        (single scope)
 *   make CFLAGS+='-DTCC_LOG_ALL=1'            (everything)
 *
 * Disabled scopes compile to nothing (dead-code eliminated).
 * ============================================================================ */

/* Master switch — enable all logging scopes at once */
#ifndef TCC_LOG_ALL
#define TCC_LOG_ALL 0
#endif

/* --- Scope switches (default to TCC_LOG_ALL) --- */

/* IR generation and optimization passes (ir/opt.c, ir/opt_jump_thread.c) */
#ifndef TCC_LOG_IR_GEN
#define TCC_LOG_IR_GEN TCC_LOG_ALL
#endif

/* Copy-propagation pass diagnostics (ir/opt.c, tcc_ir_opt_copy_prop) */
#ifndef TCC_LOG_COPY_PROP
#define TCC_LOG_COPY_PROP TCC_LOG_ALL
#endif

/* Store-load forwarding pass diagnostics (ir/opt.c, tcc_ir_opt_sl_forward) */
#ifndef TCC_LOG_SL_FWD
#define TCC_LOG_SL_FWD TCC_LOG_ALL
#endif

/* Loop optimization: induction variables, unrolling (ir/opt.c) */
#ifndef TCC_LOG_LOOP_OPT
#define TCC_LOG_LOOP_OPT TCC_LOG_ALL
#endif

/* Induction variable / strength reduction (ir/opt.c) */
#ifndef TCC_LOG_IV_SR
#define TCC_LOG_IV_SR TCC_LOG_ALL
#endif

/* Loop-invariant code motion (ir/licm.c) */
#ifndef TCC_LOG_LICM
#define TCC_LOG_LICM TCC_LOG_ALL
#endif

/* Linear scan register allocator (tccls.c) */
#ifndef TCC_LOG_LS
#define TCC_LOG_LS TCC_LOG_ALL
#endif

/* Stack frame allocation (tccls.c) */
#ifndef TCC_LOG_STACK_ALLOC
#define TCC_LOG_STACK_ALLOC TCC_LOG_ALL
#endif

/* Call site processing (arm-thumb-callsite.c) */
#ifndef TCC_LOG_CALLSITE
#define TCC_LOG_CALLSITE TCC_LOG_ALL
#endif

/* Frontend code generation — FUNCPARAMVAL processing (tccgen.c) */
#ifndef TCC_LOG_CODEGEN
#define TCC_LOG_CODEGEN TCC_LOG_ALL
#endif

/* Inline struct return expansion (tccgen.c) */
#ifndef TCC_LOG_INLINE_STRUCT
#define TCC_LOG_INLINE_STRUCT TCC_LOG_ALL
#endif

/* YAFF object format (tccyaff.c) */
#ifndef TCC_LOG_YAFF
#define TCC_LOG_YAFF TCC_LOG_ALL
#endif

/* Thumb opcode encoding trace (thumb.h) */
#ifndef TCC_LOG_THOP
#define TCC_LOG_THOP TCC_LOG_ALL
#endif

/* Thumb code generation — general (arm-thumb-gen.c) */
#ifndef TCC_LOG_THUMB
#define TCC_LOG_THUMB TCC_LOG_ALL
#endif

/* Machine-level store/assign operations (tcc.h) */
#ifndef TCC_LOG_MACH
#define TCC_LOG_MACH TCC_LOG_ALL
#endif

/* Branch size optimization (arm-thumb-gen.c) */
#ifndef TCC_LOG_BRANCH_OPT
#define TCC_LOG_BRANCH_OPT TCC_LOG_ALL
#endif

/* Scratch register management (arm-thumb-gen.c) */
#ifndef TCC_LOG_SCRATCH
#define TCC_LOG_SCRATCH TCC_LOG_ALL
#endif

/* ELF relocation processing (arm-link.c, tccelf.c) */
#ifndef TCC_LOG_RELOC
#define TCC_LOG_RELOC TCC_LOG_ALL
#endif

/* IR memory pool (ir/pool.c) */
#ifndef TCC_LOG_POOL
#define TCC_LOG_POOL TCC_LOG_ALL
#endif

/* ============================================================================
 * Core Logging Macro
 *
 * Usage:  TCC_LOG(IR_GEN, "optimized %d instructions", count);
 * Output: [IR_GEN] optimized 42 instructions
 *
 * When TCC_LOG_<scope> is 0 the entire block is dead-code-eliminated,
 * but arguments are still type-checked by the compiler.
 * ============================================================================ */

#define TCC_LOG(scope, fmt, ...)                                                                                       \
  do                                                                                                                   \
  {                                                                                                                    \
    if (TCC_LOG_##scope)                                                                                               \
      fprintf(stderr, "[" #scope "] " fmt "\n", ##__VA_ARGS__);                                                        \
  } while (0)

/* Variant without automatic newline — for multi-part messages */
#define TCC_LOG_RAW(scope, fmt, ...)                                                                                   \
  do                                                                                                                   \
  {                                                                                                                    \
    if (TCC_LOG_##scope)                                                                                               \
      fprintf(stderr, fmt, ##__VA_ARGS__);                                                                             \
  } while (0)

/* Variant with indentation — for hierarchical output */
#define TCC_LOG_INDENT(scope, indent, fmt, ...)                                                                        \
  do                                                                                                                   \
  {                                                                                                                    \
    if (TCC_LOG_##scope)                                                                                               \
      fprintf(stderr, "[" #scope "] %*s" fmt "\n", (indent) * 2, "", ##__VA_ARGS__);                                   \
  } while (0)

/* --- Per-scope convenience macros --- */

#define LOG_IR_GEN(fmt, ...) TCC_LOG(IR_GEN, fmt, ##__VA_ARGS__)
#define LOG_COPY_PROP(fmt, ...) TCC_LOG(COPY_PROP, fmt, ##__VA_ARGS__)
#define LOG_SL_FWD(fmt, ...) TCC_LOG(SL_FWD, fmt, ##__VA_ARGS__)
#define LOG_LOOP_OPT(fmt, ...) TCC_LOG(LOOP_OPT, fmt, ##__VA_ARGS__)
#define LOG_IV_SR(fmt, ...) TCC_LOG(IV_SR, fmt, ##__VA_ARGS__)
#define LOG_LICM(fmt, ...) TCC_LOG(LICM, fmt, ##__VA_ARGS__)
#define LOG_LS(fmt, ...) TCC_LOG(LS, fmt, ##__VA_ARGS__)
#define LOG_LS_INDENT(n, fmt, ...) TCC_LOG_INDENT(LS, n, fmt, ##__VA_ARGS__)
#define LOG_STACK_ALLOC(fmt, ...) TCC_LOG(STACK_ALLOC, fmt, ##__VA_ARGS__)
#define LOG_CALLSITE(fmt, ...) TCC_LOG(CALLSITE, fmt, ##__VA_ARGS__)
#define LOG_CODEGEN(fmt, ...) TCC_LOG(CODEGEN, fmt, ##__VA_ARGS__)
#define LOG_INLINE_STRUCT(fmt, ...) TCC_LOG(INLINE_STRUCT, fmt, ##__VA_ARGS__)
#define LOG_YAFF(fmt, ...) TCC_LOG(YAFF, fmt, ##__VA_ARGS__)
#define LOG_THOP(fmt, ...) TCC_LOG_RAW(THOP, fmt, ##__VA_ARGS__)
#define LOG_THUMB(fmt, ...) TCC_LOG(THUMB, fmt, ##__VA_ARGS__)
#define LOG_MACH(fmt, ...) TCC_LOG_RAW(MACH, fmt, ##__VA_ARGS__)
#define LOG_BRANCH_OPT(fmt, ...) TCC_LOG(BRANCH_OPT, fmt, ##__VA_ARGS__)
#define LOG_SCRATCH(fmt, ...) TCC_LOG(SCRATCH, fmt, ##__VA_ARGS__)
#define LOG_RELOC(fmt, ...) TCC_LOG(RELOC, fmt, ##__VA_ARGS__)
#define LOG_POOL(fmt, ...) TCC_LOG(POOL, fmt, ##__VA_ARGS__)
