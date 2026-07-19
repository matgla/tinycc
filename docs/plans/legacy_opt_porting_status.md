# Legacy Optimizer Porting Status

Two-porting tracks are in flight:

1. **Legacy → SSA structure** — moving passes from `ir/opt/ssa_opt*.c`
   into the new tree under `source/opt/ssa/` (scalar/cfg/string/memory).
2. **SSA → DSL** — rewriting each ported pass to use the DSL framework
   (`OPT_GEN_SSA` / `OPT_GEN_FLAT` / `PATTERN` / `GUARD` / `REWRITE`)
   instead of hand-written `IRSSAOptGen` tables.

Both tracks are independent: a pass can be ported to the new tree without
being on DSL, or ported to DSL without leaving `ir/opt/`. The goal is to
have every pass on both.

---

## Track 1: Legacy → SSA structure

Passes already ported to `source/opt/ssa/`:

| Pass | Location | Notes |
|------|----------|-------|
| cprop | `source/opt/ssa/scalar/cprop.c` | includes hand-written leaves |
| fold | `source/opt/ssa/scalar/fold.c` | includes hand-written leaves |
| narrow | `source/opt/ssa/scalar/narrow.c` | |
| reassoc | `source/opt/ssa/scalar/reassoc.c` | |
| strength | `source/opt/ssa/scalar/strength.c` | |
| var_imm_prop | `source/opt/ssa/scalar/var_imm_prop.c` | |
| bitop_const_fold | `source/opt/ssa/scalar/bitop_const_fold.c` | |
| branch | `source/opt/ssa/cfg/branch.c` | |
| cmp_eq_prop | `source/opt/ssa/cfg/cmp_eq.c` | |
| phi_simplify | `source/opt/ssa/cfg/phi.c` | |
| const_string_fold | `source/opt/ssa/string/const_string_fold.c` | |
| global_addr_hoist | `source/opt/ssa/memory/global_addr_hoist.c` | |
| load_cse | `source/opt/ssa/memory/load_cse.c` | |
| dce (10 sub-passes) | `source/opt/ssa/dce/` | 12 files, see below |
| loop (7 passes) | `source/opt/ssa/loop/` | 8 files, see below |
| sccp | `source/opt/ssa/scalar/sccp.c` | whole-file move |
| dead_loop | `source/opt/ssa/loop/dead_loop.c` | whole-file move |
| global_store_dse | `source/opt/ssa/memory/global_store_dse.c` | split out of the engine |
| ptr_store_dse | `source/opt/ssa/memory/ptr_store_dse.c` | split out of the engine |
| guard_collapse | `source/opt/ssa/cfg/guard_collapse.c` | split out of the engine |

**Track 1 is complete.**  `ir/opt/` now holds only `ssa_opt.c` (749 lines:
use-def chains, vreg/operand helpers, the pass driver) and `ssa_opt.h`; the
optimizer infrastructure that used to sit beside it in `ir/opt*.c` and `tccopt.c`
moved to `source/opt/{util,analysis,engine}/` (see the last batch below).

### Batch relocation 2026-07-19 — dce + loop clusters

`ir/opt/ssa_opt_dce.c` (2,127 lines) and `ir/opt/ssa_opt_loop.c` (2,061) were both
**deleted whole-file**, decomposed into 20 files under `source/opt/ssa/{dce,loop}/`
by a byte-conserving verbatim line cut (every source line lands in exactly one
destination), then comment-pruned and refactored.  Gate: object-diff **0** over the
4,257-test corpus at -O2, run three times (post-extract, post-prune, post-refactor);
`make ut` green; `make test` 13556 passed / 246 skipped / 1 xfailed.

- `dce/` — `dce.c` (driver), `temp_worklist.c`, `unreachable.c`, `dead_var_stores.c`,
  `stackloc_stores.c`, `orphan_params.c`, `ret_path_frame_store.c`, `dead_phi_cycles.c`,
  `dead_overwrite_stores.c`, `dead_global_stores.c`, `var_liveness.c`, plus
  `dce_common.c/.h` (`sl_temp_has_live_uses`, `sl_store_byte_width` — the only two
  file-statics used across sub-pass boundaries) and `dce_passes.h` (sub-pass entries).
- `loop/` — `loop_rotate.c`, `first_iter_exit.c`, `ptr_iv_exit_subst.c`,
  `loop_const_sim.c`, `loop_unroll.c`, `iv_strength_reduction.c`, `decrement_to_zero.c`,
  plus `loop_cand.c/.h` (`FieCand`, `fie_cand_cmp`, `fie_collect_members`,
  `lcs_collect_header_members` — shared by every loop driver).  The transform engines
  these drive already live in `source/opt/flat/loop/`.
- The cut required promoting those cross-boundary statics to extern; every other
  helper stayed private to its pass file.
- Comment prune: 4,577 → 3,989 lines, each file proven comment-only against a verbatim
  backup before the object-diff gate.  Refactor: dropped ~105 copied-but-unused
  `#include` / `extern` lines (each removal verified by a single-file `-Werror` compile).

### Batch relocation 2026-07-19 — ir/opt.c drain + the last SSA passes

Eighteen passes moved out of `ir/opt.c` (3,467 lines), `ir/opt_memory.c`,
`ir/opt/ssa_opt_sccp.c`, `ir/opt/ssa_opt_dead_loop.c` and `ir/opt/ssa_opt.c` by the
same byte-conserving verbatim line cut, then comment-pruned and refactored.  Gate:
object-diff **0** over the 4,257-test corpus at -O2, run twice (post-extract,
post-prune+refactor); `make ut` 4,086 tests green across 12 binaries; `make test`
13556 passed / 246 skipped / 1 xfailed.

- **New domain `source/opt/flat/ipa/`** — whole-TU analyses that feed the flat
  passes: `pure_via_sret.c`, `func_write_summary.c`, `tu_func_summary.c`,
  `tu_noreturn.c`, `tu_dead_statics.c`, plus `func_write_summary.h` and
  `tu_summary.h` for the cross-boundary types and summary lists.
- **`source/opt/flat/memory/`** gained `dead_init_via_call.c`, `stack_addr_cse.c`,
  `block_copy_init.c`, `small_memset_to_store.c`, `small_global_memset_to_store.c`,
  `mem_init.c` (the group driver), `memmove_to_indexed_stores.c`, and
  `addrof_var_fwd.c` (whole-file move of `ir/opt_memory.c`, now deleted).
- **`source/opt/ssa/`** gained `scalar/sccp.c` and `loop/dead_loop.c` (whole-file
  moves, both legacy files deleted) plus `memory/global_store_dse.c`,
  `memory/ptr_store_dse.c` and `cfg/guard_collapse.c` split out of the engine.
- `ir/opt.c` shrank 3,467 → 173 lines (pass-timing instrumentation, the FP-cache
  shims, and two vreg helpers); `ir/opt/ssa_opt.c` 1,098 → 749.
- Cross-boundary statics promoted to extern: `fws_lookup` / `fws_range_fully_set` /
  `fws_btype_bytes`; `tu_summary_head` / `tu_source_reads` / `tu_symset_{add,contains,free}`;
  `ssa_opt_global_store_dse`.
- Comment prune: 8,432 → 7,278 lines across the 22 touched files, each proven
  comment-only against a verbatim backup before the object-diff gate.  Refactor:
  dropped ~160 copied-but-unused `#include` / `extern` lines and replaced the
  inherited monolith file titles with per-pass ones.
- Wiring gained a fifth site beyond the usual four: `tests/unit/arm/armv8m/Makefile`
  needs `-I$(TOP)/source/opt/flat/ipa` on `UT_CFLAGS`, not just the new sources in
  `UT_MODULE_SRCS`.

### Batch relocation 2026-07-19 — optimizer infrastructure drain

With no passes left in `ir/opt*.c`, the remaining optimizer *infrastructure* moved
out of `ir/` and the project root by the same byte-conserving verbatim line cut.
Seven files were deleted whole (`ir/opt.c`, `ir/opt_utils.c`, `ir/opt_du.c`,
`ir/opt_alias.c`, `ir/opt_engine.c`, `ir/opt_pipeline.c`, `tccopt.c` — 3,419 lines)
and became 21 files under three new domains.  Gate: object-diff **0** over the
4,257-test corpus at -O2, run twice (post-extract, post-prune+refactor); `make ut`
4,086 tests green; `make test` 13556 passed / 246 skipped / 1 xfailed.

- **`source/opt/util/`** — the shared helper library, split by concern:
  `pass_disable.c`, `const_eval.c`, `cond_util.c`, `block_scan.c`, `purity.c`,
  `expr_equal.c`, `call_params.c`, `vreg_query.c`, `vreg_def_use.c`, `xform.c`.
- **`source/opt/analysis/`** — `du_chains.c` and `alias.c` (whole-file moves of
  `ir/opt_du.c` / `ir/opt_alias.c`).
- **`source/opt/engine/`** — `ctx.c` + `run_gens.c` (from `ir/opt_engine.c`),
  `pipeline_run.c` + `pipeline_table.c` + `gen_adapters.c` (from
  `ir/opt_pipeline.c`), `pass_timing.c` + `fp_cache_shim.c` (from `ir/opt.c`),
  and `fp_mat_cache.c` + `pass_registry.c` (from `tccopt.c`).
- Cross-boundary static promoted to extern: `opt_stats` (defined in
  `fp_mat_cache.c`, read by `pass_registry.c`).
- **Headers did not move**: `ir/opt.h`, `opt_utils.h`, `opt_du.h`, `opt_alias.h`,
  `opt_engine.h`, `opt_pipeline.h` and root `tccopt.h` stay where every consumer
  already includes them from, so no include churn outside the moved units.
- Wiring: `source/opt/Makefile` gained `CORE_OPT_SRC` / `CORE_OPT_HDRS` and three
  `$(X)source/opt/{util,analysis,engine}/%.o` rules; the old paths came out of
  `IR_FILES` / `CORE_FILES`, `UT_MODULE_SRCS` / `UT11_MODULE_SRCS` /
  `UT5_MODULE_SRCS` / `UT_COVERAGE_ONLY_SRCS`, and
  `SELFHOST_COMPILE_SOURCES`.  `scripts/bisect_opt.py` reads the `PASS_GATED`
  table from `source/opt/engine/pipeline_table.c` now.
- Comment prune: 3,710 → 3,316 lines, each file proven comment-only against a
  verbatim backup before the object-diff gate.  Refactor: 60 copied-but-unused
  `#include` lines dropped (each removal verified by a single-file `-Werror`
  compile) and every inherited monolith title replaced with a per-file one.

**`ir/` now holds only the IR core** — builder, types, pool, vregs, stack, CFG,
SSA, dump, codegen, regalloc, the instruction generators, and the optimizer
interface headers.  The only optimizer `.c` left under `ir/` is
`ir/opt/ssa_opt.c` (the SSA engine: use-def chains + pass driver).

### Batch relocation 2026-07-19 — SSA engine + pre-RA passes; `ir/opt/` retired

The last optimizer `.c` under `ir/` and the pre-RA cleanup passes embedded in the
register allocator moved out, by the same byte-conserving verbatim line cut.  Gate:
object-diff **0** over the 4,257-test corpus at -O2, run twice; `make ut` 4,086 green;
`make test` 13556 passed / 246 skipped / 1 xfailed.

- **`source/opt/ssa/engine/`** — `ir/opt/ssa_opt.c` (750 lines) split into
  `use_def.c` (chain construction + init/rebuild/free), `rewrite.c` (side-effect
  query, NOP'ing, operand/phi rewriting, replace-all-uses), `stack_resolve.c`
  (LEA/StackLoc resolution, temp→base+off, indirect stack offsets) and `driver.c`
  (target-gen registry, `ssa_opt_run_gens`, `tcc_ir_ssa_opt_run{,_target}`).
  No static→extern promotion was needed: every cross-boundary helper was already
  declared in `ssa_opt.h`.
- **`source/opt/ra/`** (new domain) — the pre-RA cleanup passes from `ir/regalloc.c`:
  `const_branch_fold.c` (with its `ra_eval_cmp_cond` / `ra_try_resolve_const_local` /
  `ra_nop_dead_block` / `ra_build_jump_target_map` helpers), `incomplete_calls.c` and
  `phi_const_chain.c`.  The three entry points lost `static` and are declared in
  `ir/regalloc.h`; `ir/regalloc.c` shrank 5,836 → 5,277 lines.
- **Dead code removed**: `ra_dead_assign_elim` — a disabled, never-called
  `__attribute__((unused))` prototype kept only for its diagnosis.  The diagnosis
  (why the linear "no use before redef" walk is unsound without a post-dominance
  check) moved to `docs/optimizations/post_ra.md`.
- **`source/opt/regalloc_pipeline.c` deleted** — 1,907 lines of an abandoned earlier
  attempt at this same relocation, never listed in any Makefile and drifted from the
  live code in `ir/regalloc.c`.
- **`ir/opt/` is gone**: `ssa_opt.h` moved to `source/opt/ssa/include/`, the five
  `#include "opt/ssa_opt.h"` sites were repointed at the basename form, and
  `-I$(TOP)/ir/opt` came out of the top-level `DEFINES` and of `UT_CFLAGS`.
- **Header relocation (same day, follow-up)**: the twelve `ir/opt*.h` / `ir/licm.h`
  interface headers and root `tccopt.h` moved to a new `source/opt/include/` root
  (`-I$(TOP)/source/opt/include` added to the top-level `DEFINES` and to `UT_CFLAGS`).
  Every consumer includes them by basename, so the move was zero-churn there; six files
  used the repo-root-relative spelling (`#include "ir/opt.h"`, `"ir/licm.h"`, …) and were
  repointed.  `ir/` now holds twelve headers, all IR-core.  **Gotcha:** the incremental
  `make cross` after a `git mv` of headers is a false green — the moved files keep their
  mtimes, so nothing relinks and stale objects hide the broken includes.  Delete the
  object tree and rebuild before trusting any gate that follows a header move.
- Comment prune: 1,489 → 1,401 lines, all `codecmp.py` code-identical.  Refactor: 120
  copied-but-unused `#include` lines dropped, the inherited `RA_DBG` macro and the
  stale "Replaces the tccls.c linear scan" header prose removed, per-file titles set.

**`ir/` is now the IR core only** — builder, types, pool, vregs, stack, CFG, SSA,
dump, codegen, regalloc, the instruction generators, and the optimizer interface
headers.  No optimization pass or engine `.c` lives there any more.

---

## Track 2: SSA → DSL

Passes already using DSL (`OPT_GEN_SSA` / `OPT_GEN_FLAT` dispatch):

| Pass | File | DSL status |
|------|------|------------|
| cprop | `source/opt/ssa/scalar/cprop.c` | Fully DSL (assign, symref_cse, load_redundant) |
| fold | `source/opt/ssa/scalar/fold.c` | Fully DSL (binary rules + hand-written leaves) |
| narrow | `source/opt/ssa/scalar/narrow.c` | Fully DSL |
| reassoc | `source/opt/ssa/scalar/reassoc.c` | Fully DSL (bin + add_cancel) |
| strength | `source/opt/ssa/scalar/strength.c` | Fully DSL |
| var_imm_prop | `source/opt/ssa/scalar/var_imm_prop.c` | Fully DSL |
| bitop_const_fold | `source/opt/ssa/scalar/bitop_const_fold.c` | Fully DSL |
| branch | `source/opt/ssa/cfg/branch.c` | Fully DSL |
| cmp_eq_prop | `source/opt/ssa/cfg/cmp_eq.c` | Fully DSL |
| phi_simplify | `source/opt/ssa/cfg/phi.c` | Fully DSL |
| const_string_fold | `source/opt/ssa/string/const_string_fold.c` | Fully DSL |
| global_addr_hoist | `source/opt/ssa/memory/global_addr_hoist.c` | Fully DSL |

Passes still using the old `IRSSAOptGen` hand-written table:

| Pass | File | Old pattern | Notes |
|------|------|-------------|-------|
| gvn | `source/opt/ssa/scalar/gvn.c` | `IRSSAOptGen` table | First DSL port candidate |

Only `gvn` remains off-DSL. The dce, loop, sccp and dead_loop passes are
dataflow/CFG engines, not opcode peepholes — not DSL candidates.

---

## Summary

- **Ported to new tree**: complete — no passes left in `ir/opt*.c`
- **On DSL**: 12/13 DSL-eligible ported passes (92%)
- **Remaining work**: 1 DSL port (gvn)

### Recommended order

1. **gvn → DSL** (in `source/opt/ssa/scalar/gvn.c`) — the last DSL gap, same
   shape as the already-ported passes (fold, cprop).

### Makefile updates needed per port

When moving a pass into `source/opt/`, five sites must be updated:

1. `source/opt/{ssa,flat}/Makefile` — add to `*_OPT_SRC`, `*_OPT_HDRS`, and
   `*_OPT_INC` if the pass lives in a new subdirectory
2. `tests/unit/arm/armv8m/Makefile` — add to `UT_MODULE_SRCS` *and*
   `UT11_MODULE_SRCS` (EASILY FORGOTTEN — causes link failures)
3. `tests/unit/arm/armv8m/Makefile` `UT_CFLAGS` — a *new subdirectory* also needs
   its own `-I$(TOP)/source/opt/...`; the UT include list is separate from
   `*_OPT_INC` and does not inherit it
4. Top-level `Makefile` — drop the old `ir/...` path from `IR_FILES`
5. `tests/selfhost/test_selfhost_compile.py` `SELFHOST_COMPILE_SOURCES` — a
   hardcoded list; a stale path fails `make test` at the selfhost smoke gate
6. A brand-new `source/opt/<domain>/` needs its own `$(X)source/opt/<domain>/%.o`
   pattern rule (the generic `%.o` rule does not `mkdir -p` the output directory)
