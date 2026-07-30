# addrof_var_fwd → SSA migration: **terminal Branch A** (assessed 2026-07-19)

`tcc_ir_opt_addrof_var_fwd`
([`source/opt/flat/memory/addrof_var_fwd.c`](../source/opt/flat/memory/addrof_var_fwd.c))
folds the inlined `__attribute__((cleanup))`/helper shape within a BB:

```
V0 <-- #C [ASSIGN]        ; constant write to local
T0 <-- &V0                ; address materialized
...copy chains (V1 <-- T0 [STORE]; T1 <-- V1 [ASSIGN])...
...T1***DEREF***...  →  ...#C...
```

Three migration attempts have now been measured. All fail. **Keep the pass** at its
`function_pipeline.c` slot (before `global_sl_fwd`).

## Current gap (2026-07-19, -O2, 4257-test corpus)

`TCC_DISABLE_PASS=addrof_var_fwd` vs. baseline (20230 funcs / 759428 instrs):

| Function | Δ | Note |
|---|---|---|
| `gcc-execute/pr57321::main` | **+9** (20→29) | the entire remaining gap |

Everything else the pass used to buy has been absorbed by `ssa:load_cse` (which carries two
explicit "flat addrof_var_fwd shape" forwarders, imm-only). One function in 20230 is left.

The pass is called directly from `function_pipeline.c`, not through the pipeline table, so
the `TCC_DISABLE_PASS` knob is checked inside the pass body — that check is now actually
present (it was documented here in 2026-07-08 but never implemented). Object-diff neutral.

## Why `pr57321` resists — the shape

After inlining `foo(&i)` into `main`, the pre-SSA IR is:

```
V0 <-- #0            ; i = 0
T0 <-- &V0           ; &i
V1 <-- T0 [STORE]    ; p = &i
T1 <-- V1 [ASSIGN]   ; reload p
CMP T1***DEREF***, GlobalSym(a)***DEREF***
```

The flat pass rewrites the compare to `CMP #0, a`. The 9-instruction prize is **not** the
compare: it is that the unresolved `*T1` read keeps alias analysis conservative, so the
140-byte `int *i[7][5] = {{0}}` init inside the `if` survives as a live `__aeabi_memset`
(plus frame growth 16 → 180 and a `push {lr}`/`pop {pc}` pair). Resolving the deref lets the
whole dead array die.

## Attempt 3 (2026-07-19): relax the two SSA guards — **no effect**

The suspected blockers were the two deliberate vetoes in
[`load_cse.c`](../source/opt/ssa/memory/load_cse.c):

* `vslot_var_forwardable` refuses addr-taken VARs;
* `vslot_track_store` skips LEA-source TEMPs ("breaks downstream stack DSE").

Both were temporarily env-gated off and measured on `pr57321` with the flat pass disabled:

| Config | `main` size |
|---|---|
| flat pass enabled | 21 |
| flat disabled | 30 |
| flat disabled + relax addr-taken | 30 |
| flat disabled + relax LEA-source | 30 |
| flat disabled + relax both | 30 |

**Zero movement.** The guards are not what blocks this; relaxing them (the fuzz-sensitive,
seed-814-class change this doc previously proposed) would buy nothing here.

The actual blocker is structural, in two independent places:

1. `ssa_opt_resolve_lea_stackloc_ex`
   ([`stack_resolve.c`](../source/opt/ssa/engine/stack_resolve.c)) walks TEMP defs only —
   every hop requires `TCCIR_DECODE_VREG_TYPE(vr) == TEMP`, and its ASSIGN hop requires
   `!src.is_lval`. `T1 <-- V1` is an **lval read of a VAR slot**, so resolution returns
   `INT_MIN` before any guard is consulted.
2. Even with resolution, the deref consumer looks the value up in `sstore_*` (keyed by stack
   offset), while a `V0 <-- #imm` VAR write is recorded — when recorded at all — in the
   separate `vslot_*` table keyed by VAR vreg. The two tables do not meet.

Closing this in SSA therefore needs a new capability — resolve a TEMP through an addr-taken
VAR *slot* to a `&VAR` value, and let the deref forwarder query VAR-slot constants — not a
guard tweak. That is a memory-model project whose measured prize is 9 instructions in 1 of
20230 corpus functions.

## Disposition

**[A★] terminal — keep flat.** Per the state machine in
[`plan_legacy_flat_ir_ssa_retire.md`](plan_legacy_flat_ir_ssa_retire.md): the residual is
proved by a *linear same-block scan with abort-on-call/jump/escape* over pre-SSA IR, which is
the Branch-A criterion ("residual needs fresh pre-SSA IR / same-block adjacency"). The pass
stays where it is, alongside the rest of the flat memory cluster (`global_sl_fwd`,
`invariant_global_load_hoist`, `store_redundant`), which are themselves terminal-A.

## Superseded attempts (kept for the record)

### Attempt 1: late SSA fixpoint pass (`ssa_opt_cprop.c`)

SSA-native implementation using `ssa_opt_resolve_lea_stackloc_ex` + a block-local
reaching-constant probe. The fold fires (verified in dumps), but recovered **0 of the +120
net** size regression of the day:

- SSA promotion (`ssa_mark_addrtaken`) runs before the fixpoint and vetoes addr-taken VARs.
  Folding the deref afterwards cannot retroactively promote V — the VAR init, LEA, and copy
  cruft stay as memory traffic (`ssa:dce:var_live` does not touch addr-taken slots).
- The consumers that turn folds into size wins are flat passes that have already run.

### Attempt 2: flat driver at regalloc entry (before ssa:reroll / CFG / SSA)

Engine + replayed consumer cascade (`global_sl_fwd`, const cascade, DCE, `store_redundant`).
Recovered 3/5 regressors but not the big ones:

| test              | legacy slot | regalloc slot |
|-------------------|------------:|--------------:|
| 20001017-2        | 80          | 80  ✓         |
| 980617-1          | 44          | 44  ✓         |
| 20000706-3        | 112         | 112 ✓         |
| pr57321           | 116         | 136 ✗         |
| 8×cleanup repro   | 72          | 124 ✗         |

Root cause: by regalloc time `invariant_global_load_hoist` has hoisted global bases and
rewritten stores as `T41 <-- GlobalSym; T41 <-- Tx STORE_INDEXED #k`. `global_sl_fwd` /
`store_redundant` only match direct `GlobalSym` STORE shapes, so the intermediate stores
survive. In the legacy order (addrof → gslfwd → iglh) the 8-block cleanup repro collapses to
a single add chain with ONE final store pair (29 IR ops); at regalloc position all 8 store
pairs + VAR cruft survive.

A regalloc-position pass also adds nothing on top of the legacy slot (supplemental run
measured: zero delta on all regressors).
