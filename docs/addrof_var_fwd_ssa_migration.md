# addrof_var_fwd → SSA migration: blocked (2026-07-08)

`tcc_ir_opt_addrof_var_fwd` (ir/opt_memory.c) folds the inlined
`__attribute__((cleanup))`/helper shape within a BB:

```
V0 <-- #C [ASSIGN]        ; constant write to local
T0 <-- &V0                ; address materialized
...copy chains (V1 <-- T0 [STORE]; T1 <-- V1 [ASSIGN])...
...T1***DEREF***...  →  ...#C...
```

Two migration attempts were measured and both fail. Keep the pass at its
tccgen.c slot (before `global_sl_fwd`) until the whole cluster migrates.

## Attempt 1: late SSA fixpoint pass (ssa_opt_cprop.c)

SSA-native implementation using `ssa_opt_resolve_lea_stackloc_ex` + a
block-local reaching-constant probe. The fold fires (verified in dumps), but
recovers **0 of the +120 net** size regression:

- SSA promotion (ir/ssa.c `ssa_mark_addrtaken`) runs before the fixpoint and
  vetoes addr-taken VARs. Folding the deref afterwards cannot retroactively
  promote V — the VAR init, LEA, and copy cruft stay as memory traffic
  (`ssa:dce:var_live` does not touch addr-taken slots).
- The consumers that turn folds into size wins are flat tccgen passes that
  have already run.

## Attempt 2: flat driver at regalloc entry (before ssa:reroll / CFG / SSA)

Engine + replayed consumer cascade (`global_sl_fwd`, const cascade, DCE,
`store_redundant`). Recovers 3/5 regressors but not the big ones:

| test              | legacy slot | regalloc slot |
|-------------------|------------:|--------------:|
| 20001017-2        | 80          | 80  ✓         |
| 980617-1          | 44          | 44  ✓         |
| 20000706-3        | 112         | 112 ✓         |
| pr57321           | 116         | 136 ✗         |
| 8×cleanup repro   | 72          | 124 ✗         |

Root cause: by regalloc time `invariant_global_load_hoist` has hoisted global
bases and rewritten stores as `T41 <-- GlobalSym; T41 <-- Tx STORE_INDEXED #k`.
`global_sl_fwd` / `store_redundant` only match direct `GlobalSym` STORE
shapes, so the intermediate stores survive. In the legacy order
(addrof → gslfwd → iglh) the 8-block cleanup repro collapses to a single add
chain with ONE final store pair (29 IR ops); at regalloc position all 8
store pairs + VAR cruft survive.

A regalloc-position pass also adds nothing on top of the legacy slot
(supplemental run measured: zero delta on all regressors).

## Actual retirement path

The pass is order-bound *within* the tccgen memory cluster. Retire it together
with its consumers as one migration:

1. `ssa:load_cse`: fold `V <-- #imm` into arithmetic-embedded `*T` derefs that
   resolve to `&V` (today only LOAD/ASSIGN-shaped reads are rewritten).
2. SSA global DSE: handle `STORE_INDEXED` off a hoisted `GlobalSym` base
   (today `ssa_opt_global_store_dse` matches direct symref STOREs only).
3. `ssa:dce:var_live`: sweep addr-taken VAR init cruft once the LEA chain is
   dead (addr-taken veto must become use-aware).
4. Then move addrof folding into the fixpoint (attempt-1 implementation is in
   git history) and delete `addrof_var_fwd` + `global_sl_fwd` +
   `invariant_global_load_hoist` + `store_redundant` from tccgen together.

Bisection: engine now has the standard knob `TCC_DISABLE_PASS=addrof_var_fwd`.
