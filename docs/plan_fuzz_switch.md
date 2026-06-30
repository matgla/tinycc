# Plan: switch profile — fuzz dense/sparse `switch` + forward `goto` to exercise jump-table codegen

> Targeted plan for **§3.3** of [`plan_fuzz_coverage_master.md`](plan_fuzz_coverage_master.md).
> Instantiates the [§2.3 template](plan_fuzz_coverage_master.md) and obeys the
> [§2.1 hard invariants](plan_fuzz_coverage_master.md) verbatim.
>
> **Profile name:** `switch`  **Feature flag:** `"switch"`
> **Oracle:** olevels (no ABI surface; self-consistency suffices)
> **Touches exactly one file:** [`tests/fuzz/gen_c.py`](../tests/fuzz/gen_c.py). All
> sweep/triage/pytest tooling already keys off `FUZZ_PROFILE`/`--profile`
> ([`triage_olevels.sh:30`](../tests/fuzz/triage_olevels.sh),
> [`batch_sweep.py:139`](../tests/fuzz/batch_sweep.py),
> [`batch_sweep.py:332`](../tests/fuzz/batch_sweep.py),
> [`test_random_c_olevels.py:66`](../tests/fuzz/test_random_c_olevels.py),
> [`test_random_c_vs_gcc.py:70`](../tests/fuzz/test_random_c_vs_gcc.py)).

---

## 1. Goal

Add one additive generator profile that emits **`switch` statements** (both a
*dense* consecutive-label form that triggers the jump-table path and a *sparse*
scattered-label form that triggers the binary-search/if-chain path) on a
**masked selector**, plus a **forward-only `goto`** over a bounded
fully-initializing region. Every arm folds a distinct value into the rolling
`cs` checksum, so any miscompile of dispatch, bounds-check, or label resolution
changes the printed output. The default `int` stream stays **byte-identical**.

---

## Status — IMPLEMENTED (2026-06-30) · bug seam OPEN

The `switch` profile is **landed in [`gen_c.py`](../tests/fuzz/gen_c.py)** and finding real bugs:

- **M1–M4 done.** `int`+`float`+`fnptr`+`bitfield` streams byte-identical (seeds {0,1,2,7,42,100,1000});
  `arm-none-eabi-gcc -O0/-O1/-O2/-Os -Wall -Wextra` compile-clean over seeds 0–40 (0 err / 0 warn);
  dense `switch` (consecutive `0..K-1`), sparse `switch` (`rng.sample` window, no dup cases),
  forward-only `goto`, and `_case_body` cs-folds — all gated; nesting bounded by `depth`.
  (`-dump-ir` not available in this non-debug `armv8m-tcc` build, so the dense→`SWITCH_TABLE`
  check is deferred to a debug build; the differential sweep covers both paths regardless —
  O0 always uses the if-chain, O1+ the table, so O0≠O1 fingers the table path.)
- **M5 done — bug seam.** olevels pre-scan **53 divergent / 5001**. Confirmed real (4-way
  cross-oracle, seed 63): `host-gcc-64 == arm-gcc-O2 == tcc-O0 == 5ca2eb2e`, both `tcc-O1`
  and `tcc-O2` wrong. Culprit knobs: **`store-load-fwd` / `const-prop` / `disp-fusion` /
  `indexed-memory` / `lea-fold`** — `store-load-fwd` is **shared with the bitfield seed-5 bug**
  (likely common root cause).
- **Harvest OPEN (M6–M8):** worklist in [`fuzz_triage_switch_0_5000.md`](../fuzz_triage_switch_0_5000.md).

---

## 2. Bug class unlocked

`switch` lowering in this fork is a multi-strategy, density-gated seam that the
fuzzer has **never sampled** (the `int`/`float` profiles emit no `switch`/`goto`
at all — see `statement()` kinds at
[`gen_c.py:346-449`](../tests/fuzz/gen_c.py)). The frontend chooses between two
fundamentally different lowerings at `block()`'s `TOK_SWITCH` handler
([`tccgen.c:25906`](../tccgen.c)):

- **Jump-table path** — gated by `switch_can_use_jump_table()`
  ([`tccgen.c:24863`](../tccgen.c)): requires `-O1+`, **≥ 4 cases**,
  **≥ 50% density** (`sw->n * 2 < range` rejects — [`tccgen.c:24877`](../tccgen.c)),
  range ≤ 65536, no `case` ranges, not `long long`. When taken, it builds a
  `TCCIRSwitchTable` ([`tccir.h:386`](../tccir.h)) via
  `tcc_ir_add_switch_table()` ([`tccgen.c:24901`](../tccgen.c)), index-adjusts
  `value - min_val` and emits `TCCIR_OP_SWITCH_TABLE`
  ([`tccir.h:191`](../tccir.h)) from `gcase_jump_table()`
  ([`tccgen.c:24946`](../tccgen.c)). The table may later be rewritten to a
  data-table load `TCCIR_OP_SWITCH_LOAD` ([`tccir.h:225`](../tccir.h)).
  Backend dispatch (LSL/ADD/LDR/ADD/BX preamble + TBB/TBH) is
  `tcc_gen_machine_switch_table_mop()` ([`arm-thumb-gen.c:3392`](../arm-thumb-gen.c)),
  with a dry-run size in `tcc_gen_machine_switch_table_dry_run_size()`
  ([`arm-thumb-gen.c:3367`](../arm-thumb-gen.c)).
- **Binary-search / if-chain path** — `gcase()`
  ([`tccgen.c:25003`](../tccgen.c)): "binary search while len > 8, else linear"
  ([`tccgen.c:25014`](../tccgen.c)), a recursive `EQ`/`GT`/`GE` comparison tree
  with delicate default-chain backpatching (the long comments at
  [`tccgen.c:25022-25048`](../tccgen.c) document a real chain-corruption hazard).

This is a **known-buggy region** — the harvested repros already cluster here:
[`bug_switch_in_loop.c`](../tests/ir_tests/bug_switch_in_loop.c) ("fails to emit
the switch dispatch when the switch is inside a while loop"),
[`bug_switch_goto_or.c`](../tests/ir_tests/bug_switch_goto_or.c) (switch + `goto`
to a common label miscompiling a wide-constant OR),
[`bug_switch_char_sparse.c`](../tests/ir_tests/bug_switch_char_sparse.c),
[`bug_switch_default_chain.c`](../tests/ir_tests/bug_switch_default_chain.c),
[`bug_switch_load_spill.c`](../tests/ir_tests/bug_switch_load_spill.c),
[`bug_switch_bitfield.c`](../tests/ir_tests/bug_switch_bitfield.c). Concretely
the unlocked classes are:

1. **Jump-table codegen** — `min_val` adjustment, bounds-check vs. table index,
   TBB/TBH offset encoding, table-in-`.rodata` placement
   ([`arm-thumb-gen.c:3392`](../arm-thumb-gen.c)).
2. **Switch-bounds-vs-loop confusion** — a `switch` nested in a bounded loop is
   the exact shape of `bug_switch_in_loop.c`; the optimizer can mis-CSE the
   selector or drop the dispatch (cf. the loop-unroll exit-branch fix in commit
   `fc4ed525`).
3. **BFS / chain over case targets** — default-target resolution + per-entry
   backpatch loop ([`tccgen.c:25998-26012`](../tccgen.c)) and the recursive
   `gcase()` default chain ([`tccgen.c:25055`](../tccgen.c)).
4. **`goto`/label resolution** — forward-jump backpatching at label definition
   ([`tccgen.c:26164-26176`](../tccgen.c)) and `TOK_GOTO` emission of a forward
   `TCCIR_OP_JUMP` ([`tccgen.c:26136`](../tccgen.c)).

Because the two lowerings diverge on a single density knob, an O1/O2 vs O0
disagreement directly fingers one path — high signal for the olevels oracle.

---

## 3. UB-freedom invariants (structural enforcement)

Mirrors the `int` profile's masking discipline (shifts/indices/`/ %`,
[`gen_c.py:215-227`](../tests/fuzz/gen_c.py)). Every rule is enforced **by
construction in the emitted text**, never by runtime luck:

1. **Default stream byte-identical.** Every new emission is gated on
   `self.has("switch")` exactly as `float` gates on `self.has("float")`
   ([`gen_c.py:353`](../tests/fuzz/gen_c.py),
   [`gen_c.py:607`](../tests/fuzz/gen_c.py)). When the flag is absent **no
   `self.rng` draw and no text** is produced. Proof obligation: diff a handful
   of `int` seeds before/after the patch and confirm byte-identity
   (Milestone M1).
2. **Selector always in the case domain.** The selector is built once as
   `sel = ((unsigned)(<expr>) & (K-1))` so it lies in `[0, K)` for a dense
   `K`-case switch whose labels are exactly `0 .. K-1`. **Every** value of the
   masked selector hits a real case → no reliance on `default`, no
   fall-through-off-the-end into undefined state.
3. **Default arm is also output-defined.** Both shapes additionally emit a
   `default:` that folds a value into `cs`, so even if the mask domain and the
   label set ever drift, every dynamic path is output-defined (defensive — the
   mask already guarantees a hit for the dense form; the sparse form *relies* on
   `default` for the masked-but-unlabeled slots).
4. **No fall-through into uninitialized reads.** Every `case`/`default` body
   ends with an explicit `break;`. Case bodies only read variables that already
   exist in `self.uvars` / `self.svars` (all initialized at declaration,
   [`gen_c.py:573-603`](../tests/fuzz/gen_c.py)) and only write through the
   existing `assign`/`checksum` statement kinds. No `case` body introduces a new
   variable whose later read could be skipped.
5. **`goto` is forward-only and never skips a needed init.** The label is
   emitted *after* the goto, over a region that contains only `cs`-folding
   statements (no new declarations), so jumping over it cannot bypass any
   initialization a later statement reads. No backward `goto` is ever emitted →
   no generated loop → bounded & terminating (master §2.1 invariant 4).
6. **No `case` value collisions.** Case labels are generated from a
   `set`/`range` so duplicates are impossible (a duplicate `case` is a *compile*
   error, not UB, but it would abort the seed and waste the run — avoid by
   construction).
7. **Selector type is plain `unsigned`** (32-bit on this target), so the
   `long long` exclusion in `switch_can_use_jump_table()`
   ([`tccgen.c:24892`](../tccgen.c)) never trips and both lowerings stay in their
   intended fast path; no signed-overflow on the selector (it is a masked
   unsigned value, master §2.1 invariant 2).

---

## 4. Emission shapes

All three shapes are emitted **only** when `self.has("switch")`. Indentation
follows the existing `pad = "  " * indent` convention
([`gen_c.py:345`](../tests/fuzz/gen_c.py)).

### 4a. DENSE switch (drives the jump table)

`K` is drawn from `[4, 8]` (≥ 4 satisfies the case-count gate
[`tccgen.c:24869`](../tccgen.c); consecutive `0..K-1` gives 100% density, far
above the 50% gate [`tccgen.c:24877`](../tccgen.c)). The selector is masked into
`[0, K)` so every dynamic value hits a real case:

```c
{
  unsigned sel7 = (unsigned)(u2 ^ (arr1[((unsigned)(u3) & 7u)])) & 7u;  /* K = 8 */
  switch (sel7) {
  case 0: cs = csmix(cs, 0x6b8u);          break;   /* distinct const per arm */
  case 1: cs = csmix(cs, (unsigned)(u2)); break;
  case 2: cs = csmix(cs, 0x1f33u);         break;
  case 3: u4 = (unsigned)(u4 + 5u) & 0xffffffffu; cs = csmix(cs, u4); break;
  case 4: cs = csmix(cs, 0x4c10u);         break;
  case 5: cs = csmix(cs, 0x2a01u);         break;
  case 6: cs = csmix(cs, 0x9e07u);         break;
  case 7: cs = csmix(cs, 0x0badu);         break;
  default: cs = csmix(cs, 0xd1eu);         break;   /* defensive; unreachable when K is a power of two */
  }
}
```

How `cs` absorbs the result: each arm runs `cs = csmix(cs, <distinct value>)`,
so a wrong dispatch (jump to the wrong case / fall through / wild jump from a bad
TBB/TBH offset) mixes a different constant and the final `checksum=` line
changes. Some arms reuse `self.block()`-style statements (assign-then-fold) so
the arm body is not always a single literal — this exercises register
allocation across the dispatch.

> **Density note.** When `K` is a power of two the mask domain equals the label
> set exactly and `default` is dead — that is the cleanest jump-table trigger.
> To also probe the bounds-check/out-of-range edge, half the time draw the mask
> as `& (P-1)` with `P = next_pow2(K) > K` (e.g. `K=5`, mask `& 7u`) so masked
> values `K..P-1` legitimately fall to `default` — still UB-free because
> `default` folds into `cs`. (Optional; gate behind a coin flip.)

### 4b. SPARSE switch (drives the if-chain / binary-search lowering)

Scattered labels with `< 50%` density force `switch_can_use_jump_table()` to
return 0 ([`tccgen.c:24877`](../tccgen.c)) → the recursive `gcase()` path
([`tccgen.c:25003`](../tccgen.c)). The selector is masked to a window that *spans*
the scattered labels, so masked values that miss every label hit `default`:

```c
{
  unsigned selX = (unsigned)(u1 + cs) & 63u;        /* window [0,64) */
  switch (selX) {
  case 3:  cs = csmix(cs, 0x77u);  break;            /* labels scattered across the window */
  case 11: cs = csmix(cs, 0xa3u);  break;
  case 28: u2 = (unsigned)(u2 ^ 9u) & 0xffffffffu; cs = csmix(cs, u2); break;
  case 49: cs = csmix(cs, 0x5eu);  break;
  case 60: cs = csmix(cs, 0xc2u);  break;
  default: cs = csmix(cs, 0x01u);  break;            /* MOST masked values land here — must be output-defined */
  }
}
```

Here `default` is **load-bearing**: most of the `[0,64)` window is unlabeled, so
the masked selector usually reaches `default`, which folds `0x01u` into `cs`.
This is the §3 invariant-3 case where the default arm guarantees every dynamic
path is output-defined. The binary-search threshold is `len > 8`
([`tccgen.c:25015`](../tccgen.c)); drawing up to ~10 sparse cases occasionally
crosses it and exercises the recursive split + default-chain backpatch
([`tccgen.c:25055`](../tccgen.c)).

### 4c. Forward `goto` over a bounded region

Forward-only, over a region with **no new declarations** (so the skip cannot
bypass an initialization a later statement reads — §3 invariant 5):

```c
{
  unsigned gsel1 = (unsigned)(u3) & 1u;
  if (gsel1) goto L1_after;       /* forward jump only */
  cs = csmix(cs, 0x33u);          /* skippable region: cs-folds only, no decls */
  cs = csmix(cs, (unsigned)(u1));
L1_after:
  cs = csmix(cs, 0x44u);          /* always reached; output-defined either way */
}
```

`cs` is folded on **both** the taken and not-taken path, so the final checksum
distinguishes "jumped over the region" from "ran the region" — a wrong forward
`TCCIR_OP_JUMP` ([`tccgen.c:26136`](../tccgen.c)) or a missed label backpatch
([`tccgen.c:26173`](../tccgen.c)) changes the output.

---

## 5. `gen_c.py` implementation sketch (exact edit points)

All edits are additive and gated on `self.has("switch")`. **No existing line in
the `int` emission path changes.**

### E1 — register the profile ([`gen_c.py:89-93`](../tests/fuzz/gen_c.py))

```python
PROFILES = {
    "int":    frozenset(),
    "float":  frozenset({"float"}),
    "switch": frozenset({"switch"}),     # adds switch/goto/labels
}
```

(Adding a key here makes `--profile switch` valid for the CLI `choices`
[`gen_c.py:657`](../tests/fuzz/gen_c.py) and threads through every downstream
tool automatically — no other file changes.)

### E2 — profile tunables (new block near the float tunables, ~[`gen_c.py:104`](../tests/fuzz/gen_c.py))

```python
# --- "switch" profile tunables -------------------------------------------
SWITCH_DENSE_MIN, SWITCH_DENSE_MAX = 4, 8     # >=4 satisfies the jump-table case-count gate
SWITCH_SPARSE_MIN, SWITCH_SPARSE_MAX = 4, 10  # sparse; up to 10 can cross gcase's len>8 split
SWITCH_SPARSE_WINDOW = 64                     # power-of-two mask window for the sparse selector
```

### E3 — a label counter (in `Gen.__init__`, alongside `self._counter`
[`gen_c.py:149`](../tests/fuzz/gen_c.py))

```python
self._label = 0          # forward-goto label bookkeeping (switch profile)
```

with a `fresh_label()` helper next to `fresh()` ([`gen_c.py:151`](../tests/fuzz/gen_c.py)):

```python
def fresh_label(self) -> str:
    self._label += 1
    return f"L{self._label}"
```

### E4 — new statement kinds in `statement()` ([`gen_c.py:344`](../tests/fuzz/gen_c.py))

Extend the `opts` list **only** under the flag (mirrors the float gate at
[`gen_c.py:353`](../tests/fuzz/gen_c.py)), and add the handlers. Crucially the
gate is added to `opts` so that when the flag is absent the `rng.choice` draw is
over the unchanged list → byte-identical `int` stream.

```python
if self.has("switch") and depth > 0:
    opts += ["switch_dense", "switch_sparse", "goto_fwd"]
```

Handlers (each returns a `list[str]` of `pad`-indented lines, like the existing
`if`/`for` handlers [`gen_c.py:401-448`](../tests/fuzz/gen_c.py)):

```python
if kind == "switch_dense":
    K = self.rng.randint(SWITCH_DENSE_MIN, SWITCH_DENSE_MAX)
    mask = K - 1 if (K & (K - 1)) == 0 else (1 << K.bit_length()) - 1
    sel = self.fresh("sel")
    lines = [f"{pad}{{ unsigned {sel} = (unsigned)({self.expr(MAX_EXPR_DEPTH)}) & {mask}u;",
             f"{pad}  switch ({sel}) {{"]
    for c in range(K):
        body = self._case_body(depth, indent + 2)   # 1-3 cs-folding/assign stmts + break
        lines += [f"{pad}  case {c}:"] + body + [f"{pad}    break;"]
    lines += [f"{pad}  default: cs = csmix(cs, {self.small_const()}u); break;",
              f"{pad}  }} }}"]
    return lines

if kind == "switch_sparse":
    n = self.rng.randint(SWITCH_SPARSE_MIN, SWITCH_SPARSE_MAX)
    labels = self.rng.sample(range(SWITCH_SPARSE_WINDOW), n)   # set => NO duplicate case values
    sel = self.fresh("sel")
    lines = [f"{pad}{{ unsigned {sel} = (unsigned)({self.expr(MAX_EXPR_DEPTH)}) & {SWITCH_SPARSE_WINDOW - 1}u;",
             f"{pad}  switch ({sel}) {{"]
    for v in sorted(labels):
        body = self._case_body(depth, indent + 2)
        lines += [f"{pad}  case {v}:"] + body + [f"{pad}    break;"]
    lines += [f"{pad}  default: cs = csmix(cs, {self.small_const()}u); break;",
              f"{pad}  }} }}"]
    return lines

if kind == "goto_fwd":
    lbl = self.fresh_label()
    g = self.fresh("g")          # 'g' prefix is hidden from uvars; never an assign target
    lines = [f"{pad}{{ unsigned {g} = (unsigned)({self.expr(MAX_EXPR_DEPTH)}) & 1u;",
             f"{pad}  if ({g}) goto {lbl};"]
    # skippable region: cs-folds ONLY, no new declarations (forward-skip is UB-free)
    for _ in range(self.rng.randint(1, 3)):
        lines.append(f"{pad}  cs = csmix(cs, (unsigned)({self.expr(MAX_EXPR_DEPTH)}));")
    lines += [f"{pad}{lbl}:;",                                  # ';' = empty stmt so a '}' may follow
              f"{pad}  cs = csmix(cs, {self.small_const()}u); }}"]
    return lines
```

### E5 — `_case_body()` helper (new method, near `block()`
[`gen_c.py:337`](../tests/fuzz/gen_c.py))

Reuses the existing statement vocabulary so case bodies exercise the same
assign/checksum machinery (and thus register pressure across the dispatch),
while **guaranteeing each arm folds into `cs`** (output-sensitive, master §2.1
invariant 3):

```python
def _case_body(self, depth: int, indent: int) -> list[str]:
    pad = "  " * indent
    lines: list[str] = []
    # 0-2 ordinary statements (assign/checksum/arraystore...) reusing self.statement,
    # then ALWAYS a distinct cs-fold so the arm is output-defined and distinguishable.
    for _ in range(self.rng.randint(0, 2)):
        lines += self.statement(max(depth - 1, 0), indent)
    lines.append(f"{pad}cs = csmix(cs, {self.rconst()});")     # distinct value per arm
    return lines
```

> **Selector construction & masking** (the load-bearing UB guard): the selector
> is `(unsigned)(<self.expr>) & MASKu`, reusing the same `self.expr(MAX_EXPR_DEPTH)`
> the rest of the generator uses ([`gen_c.py:361`](../tests/fuzz/gen_c.py)). The
> mask literal is a compile-time power-of-two minus one, exactly the index-mask
> trick at [`gen_c.py:282`](../tests/fuzz/gen_c.py). For the dense form the mask
> domain ⊆ label set; for the sparse form `default` covers the rest.

> **Label / goto bookkeeping:** labels come from `fresh_label()` (monotonic, so
> unique per program). `goto` targets are emitted *before* their label (forward
> only) and the skippable region is declaration-free. No label is ever the
> target of a backward jump, so no loop is created. Case bodies do not introduce
> declarations into `self.uvars`, so nothing a later statement reads can be
> skipped.

> **Recursion / depth:** `switch_*` and `goto_fwd` are only offered when
> `depth > 0` (same guard as `if`/`for`/`while`,
> [`gen_c.py:347`](../tests/fuzz/gen_c.py)) and pass `depth - 1` into nested
> bodies, so nesting is bounded — runs stay short (master §2.1 invariant 4).

No change to `_prologue()` is needed (no extra include or helper; `csmix` already
exists [`gen_c.py:470`](../tests/fuzz/gen_c.py)). No change to
`generate_program()` is needed (the new statements are produced inside the
existing `g.block(...)` call at [`gen_c.py:617`](../tests/fuzz/gen_c.py)).

---

## 6. Oracle

**olevels** (`test_random_c_olevels.py`), and **only** olevels for the gate.
Justification:

- A `switch`/`goto` program over masked unsigned values has **no ABI surface** —
  no struct passing, no varargs, no fnptr calling convention — so tcc-vs-gcc adds
  no coverage the self-consistency oracle lacks here. O0 is the assumed-correct
  baseline ([master §0](plan_fuzz_coverage_master.md)); O0/O1/O2/Os disagreement
  is itself the bug signal, and it pinpoints *which* lowering broke because the
  jump-table path is **O1+-only** (`switch_can_use_jump_table()` returns 0 at O0,
  [`tccgen.c:24866`](../tccgen.c)) — i.e. O0 always takes the `gcase()` if-chain,
  O1/O2 may take the jump table. An O0≠O1 divergence therefore *directly*
  fingers the jump-table path; an O1≠O2 one fingers an optimizer pass over it.
- **`-Os` carries extra value here** and is already swept by both olevels and the
  triage script ([`triage_olevels.sh`](../tests/fuzz/triage_olevels.sh) runs
  `-O0/-O1/-O2/-Os`). Jump tables are **layout-sensitive**: TBB vs TBH selection,
  literal-pool placement, and branch narrowing depend on size pressure — the same
  axis that produced tests 192 and 207 (master §6). `-Os` may pick a different
  table encoding than `-O2`, so keeping it in the comparison set is a free finder
  for layout-dependent dispatch bugs.
- vs-gcc stays available as an **escalation** oracle (run only on a confirmed
  divergence to get a third opinion / rule out a tcc-O0 baseline error), not as
  the gate.

---

## 7. Sweep & certify (exact commands, `FUZZ_PROFILE=switch`)

```bash
# 0. Build the cross compiler the harness drives.
make cross

# 1. Smoke: eyeball that the profile emits valid switch/goto C for a few seeds.
python3 tests/fuzz/gen_c.py --profile switch --seed 0
python3 tests/fuzz/gen_c.py --profile switch --seed 7
# Prove the int stream is untouched (must print NOTHING):
for s in 0 1 2 100 1000; do
  diff <(python3 tests/fuzz/gen_c.py --profile int --seed $s) \
       <(git stash show -p 2>/dev/null; python3 tests/fuzz/gen_c.py --profile int --seed $s) ;
done   # simpler: diff against a saved pre-patch copy of a few int seeds

# 2. Fast pre-scan (~200 seeds/boot, ~80% recall — FINDS, never certifies):
python3 tests/fuzz/batch_sweep.py 0 5000 --profile switch

# 3. Authoritative triage of the flagged band (one crt0 boot per seed/level,
#    full recall; writes fuzz_triage_switch_0_999.md with gcc ground truth +
#    bisected culprit knob per failing seed):
FUZZ_PROFILE=switch tests/fuzz/triage_olevels.sh 0 999

# 4. pytest wrappers (both already honor FUZZ_PROFILE):
FUZZ_PROFILE=switch pytest -s -n auto tests/fuzz/test_random_c_olevels.py
FUZZ_PROFILE=switch FUZZ_VSGCC_SEEDS=0-99 pytest -s tests/fuzz/test_random_c_vs_gcc.py   # escalation spot-check
```

**Certify:** exhaustive `FUZZ_PROFILE=switch tests/fuzz/triage_olevels.sh 0 9999`
green (0 fails) on the olevels oracle = certified band `0–9999`
(master §4 D2 GATE). `batch_sweep` is the *finder* for the advancing frontier
(`batch_sweep.py 10000 50000 --profile switch`), never the certifier — its ~80%
recall misses context-sensitive bugs (master §0).

---

## 8. Harvest

For each distinct root cause confirmed by `triage_olevels.sh` + `bisect_opt.py`
(culprit knob), distill a minimal reproducer into the `ir_tests/` corpus,
continuing the numbered sequence (**highest is currently `219`**, so start at
`219`):

- `tests/ir_tests/220_fuzz_<cause>.c` + `tests/ir_tests/220_fuzz_<cause>.expect`
  (e.g. `220_fuzz_switch_table_oob`, `221_fuzz_sparse_default_chain`,
  `221_fuzz_goto_fwd_skip`), then add each filename to `TEST_FILES` in
  [`tests/ir_tests/test_qemu.py`](../tests/ir_tests/test_qemu.py) per the
  CLAUDE.md "Adding Tests" recipe.
- Keep the minimization rooted in the buggy seam so the repro stays
  representative — sit it alongside the existing
  [`bug_switch_*`](../tests/ir_tests/) cluster.
- Write a **fuzz memory** per root cause: profile=`switch`, seed, divergent
  levels, the `bisect_opt.py` culprit knob, the exact IR/codegen site (e.g. a
  bad TBB offset in
  [`tcc_gen_machine_switch_table_mop()`](../arm-thumb-gen.c) or a default-chain
  backpatch slip in [`gcase()`](../tccgen.c)), and the fix commit — same cadence
  as the float backlog ([`fuzz_triage_float_0_100.md`](../fuzz_triage_float_0_100.md)).

---

## 9. Feature-specific false-positive traps (and how the generator avoids each)

A "divergence" must always be a real miscompile, never a legal disagreement or a
self-inflicted UB. The switch axis has four traps the generator neutralizes
**structurally**:

| Trap | Why it would be a false positive / wasted run | Structural avoidance |
|---|---|---|
| **Fall-through reading uninitialized state** | A `case` without `break;` falls into the next arm; if an arm read a var only the *first* arm initialized, the read is UB and compilers may legally differ. | Every arm ends in an explicit `break;` (E4). Case bodies declare **no** new variables — they only read pre-initialized `self.uvars`/`self.svars` and write via `assign`/`checksum` (§3 inv. 4). |
| **Backward `goto` → unbounded loop** | A `goto` to an earlier label can spin forever → QEMU hang, non-deterministic, non-terminating UB. | `goto` is **forward-only**: the label is emitted *after* the `goto`, never before (E4 `goto_fwd`). No backward edge is ever generated (§3 inv. 5; master §2.1 inv. 4). |
| **Selector outside the case domain with no `default`** | If the masked selector can exceed the labeled range and there is no `default`, the program reaches no arm and the post-switch read sees stale/undefined intent → spurious divergence. | Dense form masks into exactly `[0,K)` ⊆ labels; **both** forms always emit a `default:` that folds into `cs` (§3 inv. 2-3). Every dynamic value is output-defined. |
| **Duplicate `case` values** | A duplicate `case` is a hard *compile error* (aborts the seed, no output) — pollutes the sweep with COMPILE_FAIL noise. | Dense labels are the contiguous `range(K)`; sparse labels come from `rng.sample(range(W), n)` — a sampled **set**, so duplicates are impossible (E4). `long long` is never used, so the jump-table type gate ([`tccgen.c:24892`](../tccgen.c)) never spuriously excludes. |

Additional guard inherited from the base generator: the selector expression is
the same masked-unsigned `self.expr` used everywhere, so no signed overflow /
divide-by-zero / shift-out-of-range can sneak in through the selector
([`gen_c.py:215-227`](../tests/fuzz/gen_c.py)).

---

## 10. Milestones (ordered, checkable)

- [x] **M1 — additive wiring.** Add `PROFILES["switch"]` (E1) + tunables (E2) +
      label bookkeeping (E3). Confirm `python3 gen_c.py --profile int --seed N`
      is **byte-identical** to a saved pre-patch copy for N ∈ {0,1,2,100,1000}
      (master §2.1 inv. 1). No `switch`/`goto` handlers wired yet.
- [x] **M2 — dense switch.** Implement `switch_dense` + `_case_body` (E4/E5).
      `--profile switch --seed 0` compiles clean under `armv8m-tcc -O0/-O1/-O2/-Os`
      and gcc; manually confirm `-O1` takes the jump-table path
      (`-dump-ir` shows `SWITCH_TABLE`).
- [x] **M3 — sparse switch.** Implement `switch_sparse`; confirm it drives the
      `gcase()` if-chain (no `SWITCH_TABLE` in `-dump-ir`) and that `default` is
      hit by most selector values.
- [x] **M4 — forward goto.** Implement `goto_fwd`; confirm forward-only label
      placement and that `cs` differs between taken/not-taken paths.
- [x] **M5 — pre-scan.** `batch_sweep.py 0 5000 --profile switch` runs to
      completion; collect the flagged-seed list.
- [ ] **M6 — triage.** `FUZZ_PROFILE=switch triage_olevels.sh 0 999` produces
      `fuzz_triage_switch_0_999.md` with a culprit knob per failing seed; run
      `bisect_opt.py` on each cluster head.
- [ ] **M7 — harvest.** For each root cause: minimal `ir_tests/219+_fuzz_*.c`
      (+`.expect`), add to `TEST_FILES`, write a fuzz memory, land the fix,
      re-green the triage.
- [ ] **M8 — certify.** `FUZZ_PROFILE=switch triage_olevels.sh 0 9999` green
      (0 fails); record the certified band and open the advancing frontier
      (`batch_sweep.py 10000 50000 --profile switch`).
- [ ] **M9 — pytest gate.** `FUZZ_PROFILE=switch pytest -n auto
      test_random_c_olevels.py` green; profile is ready to layer D4 knobs onto
      (master §6).
