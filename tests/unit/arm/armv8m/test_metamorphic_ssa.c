/*
 *  test_metamorphic_ssa.c - Track 4a: IR metamorphic fuzz over the SSA passes
 *
 *  Track 4a mirrors test_metamorphic.c (legacy passes) but aims the same
 *  generator + reference interpreter at the SSA optimizer pipeline. Whether it
 *  can run in THIS isolated unit harness depends on whether the SSA passes are
 *  cleanly linkable here.
 *
 *  ── FINDING (2026-06-26): the SSA passes are NOT cleanly linkable in the
 *  isolated `make ut` harness, so this suite reports a single, explicit SKIP
 *  rather than faking a green metamorphic run. ──
 *
 *  Why:
 *   - The SSA passes do not take a TCCIRState* like the legacy passes
 *     (`int tcc_ir_opt_<name>(TCCIRState*)`). They take an `IRSSAOptCtx *ctx`
 *     (ir/opt/ssa_opt.h) which bundles:
 *         struct TCCIRState *ir;
 *         IRSSAState        *ssa;   // SSA form: phi nodes, renamed defs
 *         IRCFG             *cfg;   // basic blocks, edges, dominators
 *         IRSSAVregInfo     *vinfo; // per-vreg def/use chains
 *     None of these exist for the hand-built straight-line IR the generator
 *     produces — they are constructed by a heavy front half of the pipeline.
 *   - Standing one up means linking and *driving* tcc_ir_ssa_opt_init /
 *     tcc_ir_ssa_opt_rebuild, which pull in CFG construction (ir/cfg.c — partly
 *     linked already), dominator computation, SSA construction/renaming
 *     (ir/ssa.c), and the def-use builder. That is a second, independently
 *     bug-prone substrate: a metamorphic "failure" found through an untested
 *     SSA-construction layer could not be attributed to a single SSA pass
 *     (the whole point of Track 4), so it would manufacture false positives.
 *   - The Makefile's `UT_MODULE_SRCS` currently links only the legacy
 *     `ir/opt_*.c` passes (each a self-contained TCCIRState* transform isolated
 *     via --gc-sections). Adding the SSA pipeline is a separate harness effort
 *     (build + verify SSA construction in isolation first), tracked as future
 *     work; per the plan we register the suite and SKIP rather than fake green.
 *
 *  PATH TO ENABLING (for the tracker):
 *   1. Link ir/ssa.c, ir/dom.c (dominators), the SSA def-use builder, and the
 *      ir/opt/ssa_opt*.c TUs into UT_MODULE_SRCS.
 *   2. Add an `ssa_build.h` that constructs IRSSAState+IRCFG+IRSSAVregInfo from
 *      a generated straight-line TCCIRState (single basic block, no phis) and
 *      cross-validates that construction on hand-written cases (like the
 *      interpreter self-checks) BEFORE trusting any SSA metamorphic result.
 *   3. Reuse ir_gen.h / ir_eval.h unchanged; only the pass-driver differs
 *      (run a pass via its IRSSAOptCtx, then eval the resulting TCCIRState).
 *   The generator + interpreter + delta-reducer are pipeline-agnostic, so this
 *   is the "1-line variation" the bug-hunting plan anticipates for Phase F.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License.
 */

#include "ut.h"

/* A single test that PASSES while clearly announcing the SKIP, so `make ut`
 * stays honest: it does not assert any SSA semantics-preservation property. */
UT_TEST(test_metamorphic_ssa_skipped)
{
  fprintf(stderr,
          "  [SKIP] Track 4a SSA metamorphic fuzz: SSA passes take IRSSAOptCtx*\n"
          "         (ssa/cfg/dominators/def-use), not TCCIRState*; that substrate\n"
          "         is not linked in the isolated `make ut` harness and would need\n"
          "         independent verification first. See file header for the\n"
          "         enabling path. No SSA property is asserted here (honest skip).\n");
  return 0;
}
