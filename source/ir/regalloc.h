/*
 *  TCC IR - SSA-Aware Register Allocator
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

struct TCCIRState;

typedef struct RegAllocClass {
  int num_regs;
  const int *caller_saved;
  int num_caller_saved;
  const int *callee_saved;
  int num_callee_saved;
  int pair_align; /* 1 = pairs must be even-aligned (AAPCS) */
} RegAllocClass;

typedef struct RegAllocTarget {
  RegAllocClass int_class;
  RegAllocClass fp_class;
  int param_regs;        /* number of parameter registers (e.g. 4) */
  int static_chain_reg;  /* -1 if none */
  int frame_pointer_reg; /* -1 if none; kept out of the target's allocator map
                            but freed for allocation in functions where no
                            frame-pointer-forcing condition can fire (see
                            ra_may_need_frame_pointer) */
  int rodata_anchor_reg; /* -1 if none; allocatable, but withheld in functions
                            that address shared .rodata often enough that
                            parking its runtime base there for the whole body
                            beats reloading it per reference */
  int (*rodata_anchor_sites)(const struct TCCIRState *ir, int stop_at); /* references the anchor would serve, counted no further than stop_at; NULL = never withhold */
  int (*op_narrow_capable)(int op, int src2_is_imm, int scale); /* op has a 16-bit encoding when operands land in low regs; NULL = no narrow forms */
} RegAllocTarget;

/* References below which withholding a register for the .rodata anchor costs
 * more than it saves: the prologue load plus the register's save/restore is
 * around 8 bytes, each served reference saves 8, and the register itself is
 * worth a few bytes of encoding quality across the body. */
#define RA_RODATA_ANCHOR_MIN_SITES 2

void tcc_ir_ssa_regalloc(struct TCCIRState *ir, const RegAllocTarget *target, int spill_base);
int tcc_ir_move_coalescing(struct TCCIRState *ir);

/* Pre-RA cleanup passes (source/opt/ra/), run from tcc_ir_ssa_regalloc(). */
int ra_repair_incomplete_calls(struct TCCIRState *ir);
int ra_fold_const_branches(struct TCCIRState *ir);
int ra_fold_phi_const_chain(struct TCCIRState *ir);

