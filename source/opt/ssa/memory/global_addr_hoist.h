/*
 *  TCC SSA opt - hoist repeated global-address materializations into a vreg
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#pragma once

struct TCCIRState;

/* Hoist a global-symbol address that is materialized (pool-loaded) at several
 * call-separated use sites into a single entry ASSIGN temp, so the register
 * allocator parks it in a callee-saved register across the calls instead of
 * reloading it.  Returns the number of materializations inserted at entry.
 *
 * Runs late, AFTER phi resolution (out of SSA form): it prepends instructions,
 * which would desync the CFG from ssa->block_phis during phi resolution, but
 * block_phis is emptied by then so the caller can safely rebuild the CFG.
 * -O2 only.  See docs/plans/gap_a_ssa_var_index_addr_prop.md (gap ②). */
int tcc_ir_ssa_opt_global_addr_hoist(struct TCCIRState *ir);

/* Same idea, local scope: within one straight-line call-free region, a global
 * address materialized by two or more symref operands is materialized once into
 * a short-lived temp instead.  Break-even is 2 uses because the temp neither
 * crosses a call nor competes for a callee-saved register.  Returns the number
 * of materializations inserted. */
int tcc_ir_ssa_opt_local_addr_cse(struct TCCIRState *ir);

/* Loop-invariant variant: a global address used inside a loop is materialized
 * once in the loop's preheader instead of on every iteration.  LICM cannot do
 * this itself -- the address is a symref operand, not a movable instruction.
 * Returns the number of materializations inserted. */
int tcc_ir_ssa_opt_loop_addr_hoist(struct TCCIRState *ir);

