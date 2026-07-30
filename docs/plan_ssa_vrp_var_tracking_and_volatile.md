# Plan: ssa:vrp VAR range tracking + correct volatile handling

**Status:** IMPLEMENTED (2026-07-16) · **Depends on:** [[ssa-vrp-nopromotable-hazard-and-regreg]] session work
· **Blocks:** flat `vrp` retirement · **Related bug:** [docs/bugs/volatile-local-folded-to-constant.md](bugs/volatile-local-folded-to-constant.md)

> **Done.** Part A (volatile correctness) + Part B (stable-VAR range tracking) both landed and gated.
> - **A.3 (frontend fix) was NOT needed** — the frontend already emits real LOAD/STORE for volatile
>   locals (verified on "IR BEFORE OPTIMIZATIONS"); all folding was done by optimizer passes, the
>   register allocator (promotion), the GVN/SCCP/branch/vrp passes, DCE, and the backend spill cache.
>   `is_volatile` guards were added at every such site (all strictly volatile-gated ⇒ zero non-volatile
>   regression). See memory [[volatile-local-correctness-partA-landed]] for the full site list.
> - **B** admits a stable VAR (written-once STORE + address-never-taken + not-volatile) as a third
>   `sv_slot` region; a shared `sv_operand_slot` allows is_lval VAR-slot reads only for stable VARs.
>   A **self-loop guard** in `sv_enter` (`preds[0] == block`) was required — refining the entry block
>   of a leading `do-while` on its own back-edge is circular (folded a loop's own compare → infinite
>   loop, gcc-execute/20000519-1). Closes gcc-execute/20041114-1 with flat vrp OFF.
> - **Gate (green):** `make test` 13,550; `make ut`; gcc torture EXECUTE 5322/0-fail **with flat vrp
>   ON *and* OFF**; golden volatile 4/4; 20041114-1 passes flat-OFF at O0/O1/O2.
> - **Residual (benign, deferred):** a `volatile int x; x=7;` never-read + non-escaping store is
>   dropped by the RA (def-only interval); no observable effect for a stack local. Read/escape/
>   overwrite cases all correct.
> - **Retirement of flat `vrp` remains a separate follow-up** (re-measure residuals, then prune).

## Why

Retiring flat `vrp` (`source/opt/flat/scalar/vrp.c`) is blocked by exactly one hard
case, `gcc-execute/20041114-1` — a **link failure**, so `make test` fails with flat off:

```c
void link_failure (void);     /* defined only at -O0 */
volatile int v;
void foo (int var){ if (!(var <= 0 || ((long unsigned)(unsigned)(var - 1) < UINT_MAX))) link_failure (); }
int main (void){ foo (v); return 0; }
```

The tautology `var<=0 || (unsigned)(var-1) < UINT_MAX` must fold so `link_failure` is
eliminated (it is never defined at -O2). Standalone `foo` folds (`var` is a stable PARAM),
but `foo` is **inlined into `main`**, where `var` becomes a stack **VAR slot `V0`** (the
once-read `volatile v` spilled to memory, `is_lval`). ssa:vrp's `is_lval` guard — added this
session to fix a real deref miscompile — rejects `V0`, so the tautology is not folded → a live
`CALL link_failure` → undefined-symbol link error.

Closing it needs **VAR range tracking** in ssa:vrp. But a VAR is memory, so tracking it is only
sound when the VAR is not `volatile` — and **volatile handling is currently broken** (below). So
volatile must be made correct *first*, verified by golden IR tests, before VAR tracking can lean
on it. This is why the work spans more than vrp.

## Current state of volatile (measured on `legacyOptRemoval`, 2026-07-16)

Reproducer (docs/bugs bug, still open):

```c
int ext(int);
int f(int c){ volatile int x = 5; if (c) return ext(x); return x; }
```

At -O1/-O2 both reads of `x` fold to `#5` (mandated volatile loads eliminated — a miscompile).
Bisection with `TCC_DISABLE_PASS`:

- Disabling any single flat const-prop pass (`const_var_prop`, `value_tracking`, `copy_prop`,
  `const_prop`, `const_prop_tmp`, `var_tmp_fwd`, `known_bits`) individually does **not** stop it.
- Disabling all of them together drops `#5` from 2 → 1. **The remaining fold is done by IR
  generation itself** (tccgen.c value/SValue tracking propagates the initializer `5` into the
  reads), independent of every optimizer pass.

So there are **two** volatile-folding sites: (1) redundant flat const-prop passes, (2) the
frontend. `IRLiveInterval.is_volatile` is set in [tccgen.c](../tccgen.c) (`sym_push`, ~L1582/1617)
on `variables_live_intervals[pos]` / `parameters_live_intervals[pos]`, and the SSA DSE
([ir/opt/ssa_opt_dce.c](../ir/opt/ssa_opt_dce.c) ~L1854) + `ssa:var_imm_prop`/`cprop`
(`source/opt/ssa/scalar/cprop.c` ~L1177) already consult it — but the flag is not consulted at the
producing sites that actually fold, and the frontend fold happens before any interval guard runs.

## Part A — make volatile correct (prerequisite; touches frontend + flat + SSA)

Goal: a `volatile`-qualified local's reads and its store are never elided or const-folded, at any
`-O` level, by any pass or by IR generation. Verified by golden IR snapshots.

1. **Golden IR tests first (red).** Add `tests/ir_tests/golden/volatile/*.c` cases with `.expected`
   snapshots asserting the loads/stores survive. Cases:
   - `local_read_survives` — the bug reproducer; both reads stay memory loads, store stays.
   - `local_cmp_pair` — `volatile int x=y; if ((x<5)&&(x>5)) g();` — both `CMP` reads survive
     (today flat vrp folds this — it must NOT once volatile is correct).
   - `nonvolatile_copy_still_folds` — the 20041114-1 shape: `foo(volatile v)` inlined; the VAR
     copy `V0` is **non-volatile** and MUST still be foldable (guards the over-correction).
   Run under `make test-golden-ir`; they fail initially and pin the contract.

2. **Root-cause the interval-flag reliability** (the "is_volatile is broken" claim). Verify the
   flag is (a) actually set for every volatile VAR/PARAM in `sym_push`, and (b) **preserved across
   interval reallocation / VAR creation** — `ra_promote_multidef_temps_to_vars`
   ([ir/regalloc.c](../ir/regalloc.c)) and `ra_build_intervals` create/resize intervals and may
   zero `is_volatile`; a promoted or rebuilt VAR must inherit/recompute it. Fix any drop so the
   flag is trustworthy in the SSA region (where ssa:vrp runs).

3. **Frontend fix (tccgen.c).** Stop the frontend from treating a `volatile` local as a known
   constant value: a read of a volatile lvalue must emit a real LOAD, not the tracked immediate.
   This is the deepest change and the one that fixes the residual `#1` fold. Localize to the
   volatile-lvalue read path; do not disturb non-volatile const tracking.

4. **Audit every VAR-const producer** for the missing `is_volatile` guard, mirroring the DSE guard
   (`if (!iv || iv->addrtaken || iv->is_volatile) skip`): flat `const_var_prop`, `value_tracking`,
   `known_bits`, `copy_prop`, `const_prop`; SSA `sccp`, `var_const_fold`, `var_imm_prop`, `cprop`.
   Some already guard it — confirm all do.

5. **Gate:** golden volatile tests green; `make test` green; the two bug reproducers retain loads at
   -O1/-O2; flat-OFF O1+O2 execute torture 0 miscompiles.

## Part B — VAR range tracking in ssa:vrp (`source/opt/ssa/cfg/vrp.c`)

Builds on Part A's trustworthy `is_volatile`.

1. **Admit a "stable VAR" in `sv_slot`**, alongside the existing TEMP and stable-PARAM cases:
   a VAR is stable iff **written exactly once** (a single STORE whose dest is the VAR, `is_lval`,
   VAR-typed), **address never taken**, and **not volatile**
   (`variables_live_intervals[pos].is_volatile == 0`). A written-once, non-aliased, non-volatile VAR
   holds one value after its store, and that store dominates all reads (single def), so an
   edge-refined range on it is sound within the dominator scope — exactly like the stable-PARAM case.

2. **Slot layout.** Extend the range map to a third region: `ranges` sized
   `temp_cap + param_cap + var_cap` (`var_cap = ir->next_local_variable`); VAR pos → `temp_cap +
   param_cap + pos`. Precompute `var_stable[var_cap]` once per run (write-count + addrtaken +
   volatile), analogous to `param_stable[]`.

3. **"Written once" for a VAR ≠ PARAM.** PARAM stability counts non-lval dest writes; VAR stability
   counts **STOREs to the slot** (`dest.is_lval && vreg==VAR`). Add a `sv_var_store_count` scan (or
   fold into one precompute pass over instructions).

4. **Reuse the is_lval machinery already in place.** `sv_enter`, `sv_try_fold_jumpif/setif/select`
   currently reject `is_lval` operands. For a stable VAR read (`CMP V0,#c`, `is_lval`, VAR-typed),
   allow it *only when* `sv_slot` admits the VAR (stable). Keep rejecting TEMP-lval (pointer deref)
   and any non-stable VAR. The reg-reg `sv_same_operand` path already handles VAR-lval soundly via
   the adjacency argument — leave it; this item is about the range/edge path.

5. **Closes** 20041114-1 (inlined `main`'s `V0`) and likely other inlined-callee shapes; re-measure.

## Validation & sequencing

- **A before B, strictly.** VAR tracking that reads an unreliable `is_volatile` would miscompile
  `(volatile x<5)&&(x>5)`-style code. Do not start B until the golden volatile tests are green.
- Per-part gates: `make test` (13,550) + `make test-golden-ir` + `make ut` + flat-OFF O1+O2 execute
  torture (0 miscompiles). Fuzz deferred per project state — `make test` + golden IR + torture
  sweeps are the gate.
- **Retirement (separate, after B):** with 20041114-1 linking, re-run the flat-OFF sweep; if the
  only residuals are soft instruction-count deltas (phase-ordering `::main`, `pr92063`,
  `93_integer_promotion`, `pr49049`), decide: accept a documented residual (prior retirements did,
  e.g. first_iter_exit/20070824-1) or keep flat. Then prune per the flat-retire checklist
  (PASS_GATED entry + body + header + Makefiles + UT).

## Risks

- **Frontend volatile fix (A.3)** is the highest-risk, widest-blast change — it touches value
  tracking used by all codegen. Golden IR + full `make test` are essential; keep it minimal and
  volatile-lvalue-local-scoped.
- **Interval-flag survival (A.2)** through promotion/rename is a subtle, easy-to-miss drop.
- **VAR write-once/dominance (B.1)** assumes a single STORE dominates all reads; verify no partial
  or conditional store shape violates it (the write-once count guards multi-store; a single store
  in an unpromoted function is at the definition point and dominates uses).
- Over-correction: do not block folding of **non-volatile** VAR copies of volatile values
  (20041114-1's `V0`) — the `nonvolatile_copy_still_folds` golden case guards this.
