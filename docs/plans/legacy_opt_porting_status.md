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

Passes still living in `ir/opt/ssa_opt*.c` (not yet ported):

| Pass | Legacy file | Size | Priority |
|------|-------------|------|----------|
| dce | `ir/opt/ssa_opt_dce.c` | 68 KB | High |
| dead_loop | `ir/opt/ssa_opt_dead_loop.c` | 32 KB | Medium |
| loop | `ir/opt/ssa_opt_loop.c` | 74 KB | Medium |
| sccp | `ir/opt/ssa_opt_sccp.c` | 83 KB | High |

Total remaining: ~257 KB across 4 files.
Done: `load_cse` → `source/opt/ssa/memory/load_cse.c` (2026-07-14, faithful relocation).

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

Only one pass in `source/opt/ssa/` remains off-DSL. The 5 legacy passes
above (dce, dead_loop, load_cse, loop, sccp) are also off-DSL but they
haven't been ported to the new tree yet.

---

## Summary

- **Ported to new tree**: 12/17 passes (71%)
- **On DSL**: 12/13 ported passes (92%)
- **Remaining work**: 5 legacy ports + 1 DSL port (gvn)

### Recommended order

1. **gvn → DSL** (in `source/opt/ssa/scalar/gvn.c`) — smallest DSL gap,
   same shape as the already-ported passes (fold, cprop), good warm-up.
2. **dce** — highest-impact legacy pass, pure instruction sweep, no CFG.
3. **load_cse** — biggest legacy pass by LOC, heavy use-def machinery.
4. **sccp** — dataflow engine, most complex legacy pass.
5. **dead_loop** — loop-specific, needs loop infrastructure.
6. **loop** — loop-specific, needs loop infrastructure.

### Makefile updates needed per port

When moving a pass to `source/opt/ssa/`, three Makefiles must be updated:

1. `source/opt/ssa/Makefile` — add to `SSA_OPT_SRC` and `SSA_OPT_HDRS`
2. `tests/unit/arm/armv8m/Makefile` — add to `UT11_MODULE_SRCS`
   (EASILY FORGOTTEN — causes link failures)
3. Top-level `Makefile` if it directly references `SSA_OPT_SRC`
