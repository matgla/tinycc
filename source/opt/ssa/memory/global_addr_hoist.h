/*
 *  TCC SSA opt - hoist repeated global-address materializations into a vreg
 *
 *  Copyright (c) 2025 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

#ifndef TCC_OPT_SSA_GLOBAL_ADDR_HOIST_H
#define TCC_OPT_SSA_GLOBAL_ADDR_HOIST_H

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

#endif /* TCC_OPT_SSA_GLOBAL_ADDR_HOIST_H */
