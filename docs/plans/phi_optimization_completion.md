# Phi Optimization Completion

**Status:** planned  
**Created:** 2026-07-10  
**Goal:** make phi simplification structurally complete enough to remove
redundant live phi webs before out-of-SSA conversion, reducing parallel copies,
live ranges, and generated moves without weakening edge semantics.

---

## Current State

The phi pipeline already provides the correctness machinery expected of an SSA
compiler:

1. `ir/ssa.c` places phis through dominance frontiers and fills operands during
   SSA renaming.
2. `source/opt/ssa/cfg/branch.c` drops operands for folded or unreachable CFG
   edges while maintaining phi use slots.
3. SCCP evaluates phis over executable edges.
4. `source/opt/ssa/cfg/phi.c` removes locally trivial phis through
   `opt_dsl_phi.h`.
5. SSA DCE removes some dead phi/copy cycles.
6. `ir/regalloc.c` lowers remaining phis to scheduled parallel copies, breaks
   copy cycles, builds coalescing hints, and removes redundant copies after
   allocation.

The current phi DSL has one pattern:

```text
dest = phi(x, x, dest, undef)  ->  replace dest with x
```

It does not reason about groups of mutually dependent phis or identify two
phis that compute the same merge value.

## Invariants

Every phase in this plan must preserve these constraints:

- A phi operand belongs to a CFG edge, identified by `pred_block`; operand
  order alone is not semantic.
- Removing a phi must remove every corresponding `SSA_USE_PHI` entry and clear
  the destination's phi-definition metadata.
- Replacing a destination is all-or-nothing. If
  `ssa_opt_replace_all_uses()` cannot rewrite a protected use, the phi remains.
- All-self or all-undef phi components have no external value and remain.
- Replacement types and register classes must be compatible with `phi->btype`.
- Arbitrary copies feeding phis must not be forwarded. Those copies preserve
  edge-specific values required by out-of-SSA parallel-copy resolution.
- CFG topology and instruction indices are not changed by the phi pass.

---

## Phase 1: Repair Phi Deletion Metadata

### Problem

`opt_dsl_run_phi_rules()` currently unlinks and frees a matched phi but does not
remove its source operands from the SSA use lists or clear the destination's
`def_phi_block`. DCE sometimes repairs all chains later, but that rebuild is
conditional. Until then, stale entries can keep definitions alive or direct a
later use replacement toward a removed phi slot.

### Implementation

1. Add a shared `opt_dsl_phi_remove()` helper.
2. For every operand, remove one matching `SSA_USE_PHI(block, slot)` entry from
   its vreg info.
3. Clear the destination's `def_phi_block` when it names the removed phi block.
4. Unlink and free the phi only after metadata updates succeed.
5. Pass the block number into the helper; do not infer it from stale vinfo.
6. Reuse the helper from other phi-deletion sites where practical, especially
   dead-phi-cycle DCE, to avoid divergent maintenance logic.

### Tests

- Removing a trivial phi decrements every source use count.
- Repeated identical operands remove the correct multiplicity of use entries.
- The removed destination no longer reports a phi definition.
- A later `ssa_opt_replace_all_uses()` does not encounter the removed use.
- Protected barrel-shift uses keep both the phi and all metadata unchanged.

---

## Phase 2: Add Measurement

Optimization work should target copies that survive allocation, not raw phi
count. Add debug-only counters behind the existing RA/SSA logging facilities:

- phis before and after `ssa:phi_simplify`;
- phi operands before out-of-SSA conversion;
- trivial phis removed;
- SCC phis removed;
- congruent phis removed;
- parallel copies requested and emitted;
- cycle-breaking temporaries created;
- copies coalesced and copies still reaching codegen;
- spills attributable to intervals participating in phi copies.

Collect baselines for the IR suite, GCC torture execute tests, and selected
loop/switch benchmarks. Keep instrumentation compile-time gated and avoid
unconditional output.

---

## Phase 3: Trivial Phi SCC Elimination — LANDED

Landed in `source/opt/ssa/cfg/phi.c` (`phi_scc_eliminate_once` /
`phi_scc_remove_component`, Tarjan SCC) and driven from the
`ssa_opt_phi_simplify` fixpoint loop. Full regression coverage lives in
`tests/unit/arm/armv8m/test_ssa_opt_phi.c` (two/three-phi cycles collapse,
two-external-value kept, all-self/undef kept, incompatible-type kept, atomic
protected-use kept, use-count/def-metadata correctness). Validation: 282/282
ssaopt unit tests pass; `make test` green (13529 passed, 246 skipped, 1 xfailed).

### Missing Case

Local simplification cannot reduce a mutually recursive component:

```text
A = phi(x, B)
B = phi(x, A)
```

The component has one external value, `x`, so both `A` and `B` are equivalent
to `x`. Keeping the component creates unnecessary loop-carried intervals and a
parallel-copy cycle.

### Algorithm

1. Build a map from phi destination vreg to its `IRPhiNode` and block.
2. Build phi dependency edges when an operand is another phi destination.
3. Compute strongly connected components with Tarjan's or Kosaraju's
   algorithm.
4. For each component, gather distinct external, defined operand vregs:
   - ignore negative/undef operands;
   - ignore operands whose definitions belong to the component;
   - preserve predecessor information for diagnostics and validation.
5. A component is removable only when it has exactly one external vreg and all
   phi types are compatible with that replacement.
6. Preflight replacement of every component destination. If any destination
   has a protected use, leave the entire component unchanged.
7. Replace all component destinations, then remove all component phis through
   the Phase 1 helper.
8. Repeat until no component changes, because collapsing one component can
   expose another.

The existing per-phi DSL runner cannot express atomic group rewrites. Add a
separate structural runner or a phi-group action; do not hide partial mutation
inside a single `OPT_GEN_PHI` callback.

### Tests

- Two-phi and three-phi cycles with one external value collapse.
- Components with two distinct external values remain.
- All-self/all-undef components remain.
- A protected use on any member preserves the whole component.
- Components spanning loop header and latch relationships collapse safely.
- Source use counts, destination definitions, and downstream operands are
  correct after atomic removal.

---

## Phase 4: Congruent Phi Elimination — LANDED

Landed in `source/opt/ssa/cfg/phi.c` (`phi_congruent` /
`phi_congruent_eliminate_once`), wired into the `ssa_opt_phi_simplify` joint
fixpoint alongside trivial + SCC. Operands are matched by `pred_block`
(not slot order) with exact vreg equality; btype must match; the first phi in
the block list is kept as representative; removal is preflighted with
`ssa_opt_can_replace_all_uses` and performed through the Phase 1 helper.
Coverage in `tests/unit/arm/armv8m/test_ssa_opt_phi.c`: exact merge with
use redirection, differing edge value kept, swapped predecessor mapping kept,
type mismatch kept, protected-use kept. Validation: 287/287 ssaopt unit tests
pass; `make test` green (13529 passed, 246 skipped, 1 xfailed).

### Missing Case

Two phis in the same merge block may have identical edge mappings:

```text
A = phi(p0: x, p1: y)
B = phi(p0: x, p1: y)
```

`B` can use `A` as its representative. Instruction GVN does not value-number
phi nodes.

### Algorithm

1. Canonicalize each phi as `(btype, sorted(pred_block, operand_vreg)...)`.
2. Hash phis per destination block; do not compare operand slot order alone.
3. Require compatible btype, width, floating-point class, and complex flags.
4. Keep the first dominating phi as the representative.
5. Preflight `ssa_opt_replace_all_uses()` safety before deleting a duplicate.
6. Remove duplicates through the Phase 1 helper.
7. Run local trivial simplification afterward because merging uses can expose
   new trivial phis and dead definitions.

Start with exact vreg equality. Do not initially use copy propagation or GVN
equivalence for operands; edge copies can carry necessary out-of-SSA semantics.

### Tests

- Exact congruent phis merge.
- Different predecessor-to-value mappings do not merge.
- Same operand list with different predecessor mapping does not merge.
- Type or register-class mismatch does not merge.
- Protected representative replacement leaves the duplicate intact.

---

## Phase 5: Reassess Codegen Opportunities — DONE

### Measurement corpus

`TCC_LOG_IR_GEN` + `TCC_LOG_LS` counters aggregated over the GCC torture suite
compiled at `-O2` (`-nostdinc`): 3616 translation units compiled, 19,531
functions allocated, 12,021 of them with at least one phi operand.

### Phi simplification is saturated (Phases 1-4 have ~0 further headroom)

Over 79,026 phis seen by `ssa:phi_simplify`:

| removal | count | share of phis |
|---------|-------|---------------|
| trivial | 197 | 0.25% |
| SCC (Phase 3) | 8 | 0.010% |
| congruent (Phase 4) | 0 | 0.000% |
| **total** | **205** | **0.26%** |

Renamed SSA almost never contains a redundant mutually-recursive component or
a pair of exactly-congruent phis, so Phases 3-4 are correctness/completeness
insurance, not a code-size lever. **No further phi structural pass is worth
building.** Phi→`SELECT` remains out (the flat optimizer already forms
profitable diamonds; late conversion would extend live ranges).

### The cost has moved to out-of-SSA copy lowering

78,190 phi copies are emitted at predecessor block ends. Their fate:

| fate | count | share |
|------|-------|-------|
| graph-coalesced (become identity, elided free) | 23,888 | 30.6% |
| move-coalesced post-RA | 12,337 | 15.8% |
| **reach codegen as real register moves** | **41,965** | **53.7%** |

So **more than half of all phi copies survive as actual instructions.** Copy
cycles are a non-problem: `cycle_temporaries = 0` across the entire corpus —
the plan's cycle-related RA candidate is retired. `participant_spills = 4,170`
(phi-copy intervals that also spilled) is a secondary cost.

Surviving copies are moderately concentrated: 38% of functions emit zero, but a
heavy tail (127 functions with 11+, max 1,442 in one function) holds
disproportionate weight; the top 10% of functions carry 33% of surviving
copies.

### Recommendation — next work is RA coalescing, not phi passes

Ranked by leverage:

1. **Raise copy-coalescing coverage.** The 53.7% (~42K moves) that survive
   `ra_coalesce_graph` + `tcc_ir_move_coalescing` are the single largest lever.
   First step is diagnostic: add rejection-reason counters (interference,
   call-crossing, hint conflict, already-coalesced) to both coalescers, re-run
   this corpus, and attack the dominant class.
2. **Weight coalescing affinity by loop depth / edge frequency.** Prefer
   eliminating hot backedge copies over cold exit copies; today hints are
   frequency-blind. Targets dynamic cost even where static count is flat.
3. **Cut phi-participant spills (4,170).** Bias the allocator to keep phi-copy
   participants in registers, or place their spill slots so the copy degrades
   to a direct stack move.

Do **not** invest further in: phi structural passes, phi→`SELECT`, or
cycle-breaking machinery (0 cycles observed).

---

## Validation

Each behavioral change requires a regression test before the implementation.
Run after every phase:

```bash
make cross -j$(nproc)
make -C tests/unit/arm/armv8m build_ssaopt/run_unit_tests_ssaopt
ASAN_OPTIONS=detect_leaks=0 tests/unit/arm/armv8m/build_ssaopt/run_unit_tests_ssaopt
make test -j16
```

For codegen changes, also compare instruction counts and object sizes against
the pre-change baseline. A transformation is accepted only when it has:

- zero new fuzz divergences;
- no IR or unit regressions;
- no increase in cycle-breaking temporaries;
- a measurable reduction in emitted phi copies, moves, spills, or code size on
  at least one representative workload;
- no material compile-time regression on large CFG torture tests.

## Deliverables

1. Correct shared phi-removal metadata helper and focused unit coverage.
2. Gated phi/copy statistics.
3. Atomic trivial-phi SCC simplification with regression tests.
4. Exact congruent-phi elimination with regression tests.
5. Updated `docs/optimizations/ssa_passes.md` and DSL documentation.
6. Before/after code-size, copy, spill, and fuzz results.

## Non-Goals

- Replacing the existing parallel-copy scheduler.
- Forwarding arbitrary instruction copies into phi operands.
- Rebuilding CFG topology inside the phi pass.
- Speculative PRE or expression factoring through phis.
- Enabling broader dead loop-phi removal without an edge-sensitive proof.
