# Gap A: SSA constant propagation of scalar-local index chains into address arithmetic

**Status:** planned · **Branch:** `legacyOptRemoval`

## Update (refined diagnosis)

Re-measured after `ssa:const_string_fold` landed: test_array_ptr is tcc **818** vs
arm-none-eabi-gcc -O2 **466**. The VAR-index chain (`i0..i5`) actually **does** fold in
SSA (`V0→#0`, verified: isolated it converges by driver iteration 4). The residual 94 MLA
are the **volatile-index** address computations (`&a[i][j]+vN`), which neither compiler can
fold — GCC just lowers them with add/shift instead of MLA (a codegen-quality difference).

The real gap is **calls**: tcc keeps 35 `__tcc_strlen` + 35 printf + 35 abort (105 `bl`) vs
GCC's 20+19+19 (58). Of the 35 unfolded `__tcc_strlen`:
- **6 are cleanly foldable** — the subtraction forms `*(&a[i][j] − k)` reduce to
  `GlobalSym+addend SUB #const` (e.g. `GlobalSym(a)+28 SUB #28` = `a+0`). tcc does not fold
  `symref+addend ± #imm → symref+(addend±imm)`, so the address stays a runtime `SUB`,
  `ir_opt_eval_const_string` can't resolve it, and strlen never folds. **This fold works in
  isolation but does NOT fire in the full function** (not the driver iteration cap —
  bumping `max_iterations` 5→20 changed nothing). So it's a convergence/ordering bug: the
  `symref+addend±imm` form is produced late (after the index chain folds), past whichever
  pass last folds it, or a per-pass bail on the large function. **← highest-ROI next step.**
- **29 are genuinely runtime** (`…MLA #28 + T` volatile indices, pointer derefs). GCC keeps
  these too; the gap is address-lowering quality (tcc 198 ldr/151 movs/94 mla vs GCC
  111/75/0), diffuse and lower per-instruction ROI.

**① LANDED.** Root cause: in the full function `i1` folds to `#1` only in the SSA phase,
producing `GlobalSym+addend SUB #const`, but nothing in SSA folded a *symref-addend ± imm*
(the flat passes only handle the literal-constant case, which is why it worked in
isolation). Fix: `fold_binary` (ir/opt/ssa_opt_fold.c) now folds
`symref+addend ± #imm → symref+(addend±imm)` into a plain constant symref; the driver loop
then lets `ssa:const_string_fold` collapse the enclosing strlen next iteration. Result:
**test_array_ptr 818 → 729** (−89 instrs; 6 strlen + 6 printf + 6 abort calls removed).
Pinned by runtime IR test `353_ssa_symref_addend_fold.c`; `make test` green (13505).

**Remaining (②) — pinned, NOT a fold; it's register allocation.** After ①, all 29 kept
strlen calls are genuinely runtime (volatile addresses) — GCC keeps them too, so there is
**no remaining fold-based win** for this test. tcc 729 vs GCC 466 is dominated by
**rematerialization of global addresses**: tcc issues 120 pc-relative pool loads vs GCC's
50, reloading the same address up to **13×** (e.g. `&a`, `&v0..&v7`, printf format strings).
Root cause: every `A()` site calls strlen/printf/abort, which clobber the caller-saved
registers, so a global address materialized into a caller-saved reg is dead after each call
and tcc reloads it. GCC instead parks a frequently-used global address in a **callee-saved**
register for the whole function (surviving the calls). The backend has `cached_global_sym`/
`cached_global_reg` + `imm_cache[16]`, but they're invalidated at block/call boundaries (the
macro expands to ~87 blocks), so the cache never spans the calls.

Closing ② means an RA change: dedicate a callee-saved register to a hot global address across
its whole live range (spanning calls), i.e. rematerialize-vs-keep-in-reg costing. That is a
substantial, correctness-sensitive register-allocation feature (regalloc changes are the
repo's biggest bug source), diffuse in payoff, and specific to global-heavy code like this
torture test. Recommend scoping it as its own project rather than a strlen-4 follow-on.

**② LANDED** as `ssa:global_addr_hoist` (`source/opt/ssa/global_addr_hoist.c`). Rather than a
raw RA change, it reuses the existing RA machinery: it hoists a global address that is reloaded
from the literal pool at several call-separated use sites into a single `ASSIGN` temp at the
function entry (an `ir_vreg_alloc_temp` vreg), and rewrites the uses to reference it. The
existing linear scan already forces a call-crossing vreg into a callee-saved register — so the
address is materialized once, parked in r4-r11 for the whole function, and reused across the
calls, matching GCC. Key facts learned building it:

- **Global addresses are NOT vregs** — a SYMREF operand is a constant the backend materializes
  on-demand into a scratch reg with a peephole `imm_cache` that resets at every call. The RA
  never sees a value to park; hoisting into an `ASSIGN` temp is what exposes it.
- **Deref uses are the big win.** The strlen-4 residual is `GlobalSym(vN)***DEREF*** MLA #28 + T`
  — a global pointer dereferenced inside address arithmetic (reloaded 8-13x). The hoist
  materializes the *address* (`is_lval=0`, a link-time constant) and preserves each use's own
  `is_lval` bit, so `[&vN]` becomes `[Tbase]` (load-through-register). The simple `is_lval=0`
  cases are already hoisted by the frontend/fusions.
- **Runs AFTER phi resolution (out of SSA).** Prepending entry ASSIGNs shifts every instruction
  index; mid-SSA that desyncs the CFG from `ssa->block_phis` and breaks phi-copy placement.
  Post-resolution `block_phis` is emptied, so the caller safely rebuilds the CFG (resizing the
  now-empty `block_phis` to the new block count) before `ra_build_intervals`.
- **Whitelist the hoisted ops.** `BLOCK_COPY` requires a literal symref source (memcpy
  relocation); a `FUNCCALL` target is a `bl` reloc. Only ops that lower a symref operand into a
  general register (ALU / CMP / LOAD·STORE base / LOAD_INDEXED base / FUNCPARAMVAL / RETURNVALUE)
  are hoistable.
- **Cost model: reload_estimate ≥ 5, cap 4.** A lower threshold hoists marginal (3-4 reload)
  addresses that compete for callee-saved regs and tip the allocator into spills — measured at
  +1108 bytes on one bitfield-struct function (20040709-1) at threshold 3. Threshold 5 keeps the
  strlen-4 wins (reload 8-13) and nets **−380 bytes across 359 torture files, 0 regressions**.

Result: **strlen-4 test_array_ptr 2260 → 2140 bytes** (120 pool loads → 62, max reload 13 → 1;
GCC parity target ~50). `-O2` only. `make test` green (13506). Pinned by IR test
`354_ssa_global_addr_hoist.c`; disable knob `TCC_DISABLE_PASS=ssa:global_addr_hoist`.

## Problem (original framing — VAR propagation; now shown to already work)

`ssa:const_string_fold` (docs/plan_ssa_const_string_fold.md) is the consumer; it only
fires when a string-address argument is already a constant `GlobalSym+addend` at SSA
time. In `gcc-execute/strlen-4:test_array_ptr` the addresses are `&a[i][j] ± k` where
`i0..i5` is a constant ladder (`i0=0; i1=i0+1; …`). Flat `const_prop` folds this whole
ladder into the address; the SSA passes do not — after the entire SSA phase the IR still
holds `V0 <- #0` known but `T <- V0 MLA #84 + …` unfolded. Net: strlen-4 stays +234.

Root cause: `i0..i5` are scalar-local **VARs** (stack slots). `ssa:sccp`/`ssa:cprop`/
`ssa:var_imm_prop` know `V0=#0` but do not substitute it into arithmetic operands
(`MUL`/`MLA`/`ADD`) at real-function scale, so the address never collapses to a constant.

## Goal

Make the SSA phase propagate a single-def, non-address-taken scalar-local VAR's constant
value into every arithmetic operand position, so `V0 MLA #84 + T` with `V0=#0` folds to
`T` and `&a[i][j]±k` reduces to `GlobalSym+addend`.

## Approach (smallest first)

1. **Measure where it stalls.** `TCC_DISABLE_PASS=const_prop` + `-dump-ir-passes=ssa:cprop`
   / `ssa:var_imm_prop` on the strlen-4 slice; confirm which pass *should* substitute
   `V0` into `V0 MUL/MLA #k` and doesn't.
2. **Extend the owning pass** (likely `ssa:var_imm_prop` or `ssa:cprop`): when a scalar
   local VAR has a single reaching const def and is not address-taken, rewrite its value
   uses inside `MUL`/`MLA`/`ADD`/`SUB` src positions to the immediate. Let existing
   `ssa:fold`/`ssa:reassoc` collapse the now-constant arithmetic and fold into the symref
   addend. Reuse existing const-def / address-taken queries — do not hand-roll aliasing.
3. **Transitive ladder.** `i1=i0+1` etc. must fold too; rely on the driver fixpoint
   (`i0→#0` ⇒ `i1` def becomes `#0+1` ⇒ next round folds). Verify the fixpoint converges
   at scale (36 sites) — if not, that's the real bug, not a per-op gap.

## Guards / risk

- Only single-def, non-address-taken scalar locals (a store through the slot's address, or
  `&i`, forfeits it). medium risk — this is classic SCCP-over-promoted-locals; the danger
  is over-propagating a VAR that is actually reassigned or aliased.
- Do not touch VARs feeding `LOAD_INDEXED`/deref-as-address without the existing lval
  guards (see [[ptr-load-cse-lval-var-redef]] / [[slfwd-320164-store-store-dse-var-dest]]).

## Validation

- Gate: `make test -j16`; A/B `TCC_DISABLE_PASS=const_prop` per-function objdump on
  strlen-4 (`test_array_ptr` OFF 810 → 585 target); `compare_worktree.py --baseline-commit
  HEAD --opt o2`. Add UT11 tests for the VAR-into-MLA fold.
