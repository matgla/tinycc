# Master plan: extending differential-fuzz coverage

> **Scope.** This is the *umbrella* plan for growing what the differential fuzzer
> reaches. It catalogs **every** extension direction in one place, gives the
> **reusable authoring contract** each new generator profile must satisfy, and
> sequences the work. Each axis below is deliberately specified to a depth that a
> later **targeted plan** (`docs/plan_fuzz_<axis>.md`) can pick up and implement
> without re-deriving the framework.
>
> Companions (already in `docs/`):
> - [`fuzz_triage_guide.md`](fuzz_triage_guide.md) — sweep/triage cadence once a divergence is found.
> - [`debugging_fuzz_divergences.md`](debugging_fuzz_divergences.md) — root-cause workflow (`bisect_opt.py`, culprit knobs, IR dumps).
> - [`plan_fuzz_reach_expansion.md`](plan_fuzz_reach_expansion.md) — the original reach argument + per-axis bug-density ranking. **This master plan supersedes it as the index**; that doc remains the rationale reference.

---

## 0. Where we are (baseline)

| Component | State |
|---|---|
| Generator [`tests/fuzz/gen_c.py`](../tests/fuzz/gen_c.py) | UB-free, seedable, **profile**-parameterized. Profiles: `int` (default, frozen), `float` (landed). |
| Oracle A — olevels | [`test_random_c_olevels.py`](../tests/fuzz/test_random_c_olevels.py): O0/O1/O2 self-consistency. `tcc -O0` = assumed-correct baseline. **Blind to O0-WRONG.** |
| Oracle B — vs-gcc | [`test_random_c_vs_gcc.py`](../tests/fuzz/test_random_c_vs_gcc.py): `arm-none-eabi-gcc -O2` gold. Catches O0-WRONG. Runs only a 12-seed smoke by default. |
| Triage | [`triage_olevels.sh`](../tests/fuzz/triage_olevels.sh) (exhaustive, authoritative), [`batch_sweep.py`](../tests/fuzz/batch_sweep.py) (fast pre-scan, ~200 seeds/boot, **~80% recall** — misses context-sensitive uninit-read bugs), `bisect_opt.py` (culprit knob). |
| Harvest | 66 regression tests `ir_tests/185_*`…`218_*`, each with a fuzz memory. |

### Implementation status — ALL 6 D1 profiles landed (2026-06-30)

Every feature-space profile from §3 is now implemented in `gen_c.py` (additive; the `int`/`float`
streams stay byte-identical). Each was validated (M0 byte-identity + `gcc -Wall -Wextra`
compile-clean) and swept on its oracle(s):

| Profile | olevels (0–5000) | vs-gcc (0–999) | Status / headline |
|---|---|---|---|
| `fnptr` | 0 | 0 | clean |
| `bitfield` | 162 | 30 | real `-O2` bug · culprit `store-load-fwd` |
| `switch` | 53 | — | real `-O1/-O2` bug · culprit `store-load-fwd` |
| `struct_byval` | 2 | 1 | **HardFault crash** · *sole* culprit `store-load-fwd` |
| `varargs` | 0 | 0 | clean |
| `ptr` | **183** | — | densest seam · SSA-pipeline + const-prop; address-safe (I9 audited) |

**Cross-cutting finding — `store-load-fwd` is the dominant root cause.** Store→load forwarding
is the QEMU-confirmed culprit across **three** profiles (bitfield, switch, struct_byval), and for
the struct_byval **crash** it is the *sole* culprit — making that the cleanest reproducer and the
**single highest-leverage fix in the whole sweep**. Per-profile worklists:
[`fuzz_triage_bitfield_0_5000.md`](../fuzz_triage_bitfield_0_5000.md),
[`fuzz_triage_switch_0_5000.md`](../fuzz_triage_switch_0_5000.md),
[`fuzz_triage_struct_byval_0_5000.md`](../fuzz_triage_struct_byval_0_5000.md),
[`fuzz_triage_ptr_0_5000.md`](../fuzz_triage_ptr_0_5000.md). **Next phase = harvest** (D1 reach is done).

**The limiter is no longer triage tooling — it is what the generator samples.**
Today's `int`/`float` profiles emit *none* of: function pointers, bitfields,
packed structs, `switch`/`goto`, struct-by-value, varargs, or pointer aliasing.
Each is a whole optimizer bug-class that has *never been fuzzed*.

---

## 1. The four extension dimensions

Coverage grows along four orthogonal dimensions. A targeted plan picks one cell.

```
        ┌────────────────────────────────────────────────────────────────┐
  D1    │ FEATURE SPACE   new --profile per construct (highest leverage)  │
        │   float✅  fnptr  bitfield  switch  struct-byval  varargs  ptr   │
        ├────────────────────────────────────────────────────────────────┤
  D2    │ SEED RANGE      certify 0–9999 exhaustive · advance frontier    │
        ├────────────────────────────────────────────────────────────────┤
  D3    │ ORACLE          promote vs-gcc to a swept gate · O0-WRONG escal. │
        ├────────────────────────────────────────────────────────────────┤
  D4    │ OPT PRESSURE    -fno-<pass> / TCC_SKIP_SSA combos as *finders*   │
        └────────────────────────────────────────────────────────────────┘
```

- **D1 is the headline** — each new profile unlocks a bug class no amount of
  D2/D3/D4 can reach, because the relevant C never gets emitted.
- D2 is cheap and primes the pump (more of the *current* slice).
- D3 closes the structural blind spot (bugs where *all* tcc levels agree but are wrong).
- D4 is a multiplier — it turns the bisection knobs into finders.

---

## 2. The profile authoring contract (the reusable scaffold)

Every D1 axis is "add one profile." The mechanism already exists and is purely
additive — **follow this contract and the existing seed→bug corpus stays valid.**

### 2.1 Hard invariants (non-negotiable)

1. **Never edit the default (`int`) emission stream.** Generation is one
   `random.Random(seed)` stream; drawing *any* extra rng value reshuffles it and
   rots every seed number in the triage tables + fuzz memories. New emission is
   gated on a feature flag so that when the flag is absent, **no rng value is
   drawn and no text is emitted**. (`float` already obeys this — diff a few `int`
   seeds before/after to prove byte-identity.)
2. **UB-freedom by construction.** Any divergence must mean a *real* miscompile,
   never a legal disagreement between two conforming compilers. Each profile
   states its UB invariants (see template) and enforces them structurally, the
   way the integer profile masks shifts/indices and guards `/ %`.
3. **Output-sensitive.** Every value the new construct produces must flow into
   the rolling `cs` checksum (directly, or via the `memcpy` bit-reinterpret trick
   the float profile uses for FP), so a wrong result changes printed output.
4. **Bounded & terminating.** No unbounded loops, no recursion cycles, no
   unbounded allocation — QEMU runs must stay short and deterministic.

### 2.2 End-to-end wiring (already plumbed — a new profile is ~one place)

```
gen_c.py     PROFILES["<name>"] = frozenset({"<feat>"})   # add the profile
             Gen.has("<feat>")  gates every new emission   # in statement()/
                                                           # _prologue()/generate_program()
                 │ threads automatically through ↓ (no edits needed):
triage_olevels.sh   FUZZ_PROFILE=<name>   → fuzz_triage_<name>_<lo>_<hi>.md
batch_sweep.py      --profile <name> / FUZZ_PROFILE
test_random_c_olevels.py   FUZZ_PROFILE=<name>
test_random_c_vs_gcc.py    FUZZ_PROFILE=<name>
gen_c.py --profile <name> --seed N         # inspect one program
```

The *only* file a new profile touches is `gen_c.py`. Everything downstream keys
off `FUZZ_PROFILE`/`--profile` already (verified: `triage_olevels.sh:30`,
`batch_sweep.py:139/332`, both pytest wrappers).

### 2.3 Per-profile targeted-plan template

Each `docs/plan_fuzz_<axis>.md` fills in:

```
## Profile: <name>            feature flag: "<feat>"
UB invariants     : <the structural rules that keep it UB-free>
Emission shapes   : <the concrete C constructs + how cs absorbs their result>
Bug class unlocked: <the optimizer/backend seam it exercises>
Oracle            : olevels | vs-gcc | both   (and why)
Sweep             : FUZZ_PROFILE=<name> tests/fuzz/triage_olevels.sh 0 999
                    python3 tests/fuzz/batch_sweep.py 0 5000 --profile <name>
Certify           : exhaustive 0–N green on the chosen oracle(s)
Harvest           : ir_tests/NN_fuzz_<cause>.c (+ .expect) per root cause, + memory
```

---

## 3. D1 — feature-space profiles (ranked by expected bug density)

Do **top-down; harvest fixes before opening the next** (the plan discipline).
Each row is a future targeted plan.

### 3.0 `float` — ✅ LANDED, backlog OPEN
Three-address FP ops + compares, memcpy-folded. Found bugs immediately (float
seeds 0–100 nearly all diverge; first cluster bisects to `-fno-const-prop`).
**Before opening 3.1, harvest the float backlog** (`fuzz_triage_float_*.md`) into
fixes + `ir_tests/` regressions. Optional deepening (own mini-plan): FMA/`a*b+c`
contraction shapes, int↔float conversions across widths, more loop-carried FP.

### 3.1 `fnptr` — function pointers / indirect calls  *(plan rank #2)*
- **Shapes:** a static dispatch table of the already-emitted DAG helpers
  (`unsigned(*tab[N])(unsigned,unsigned)`), called via `tab[idx & (N-1)](a,b)`;
  result into `cs`.
- **UB invariants:** all targets share the exact prototype; index masked to
  `[0,N)`; table fully initialized; no escaping/comparison of pointers.
- **Bug class:** indirect-call ABI, sret-through-fnptr, stack-arg delivery,
  fn-ptr CSE/devirtualization.
- **Oracle:** vs-gcc (ABI-shaped). Harvest: continue `ir_tests/219_*`.

### 3.2 `bitfield` — bitfields & packed structs  *(plan rank #3)*
- **Shapes:** `struct { unsigned a:N; unsigned b:M; … }` plus `#pragma pack(1)` /
  `__attribute__((packed))` variants; read/write fields into `cs`.
- **UB invariants:** unsigned bitfields only; values masked to field width;
  fully initialized; no reads across the union of overlapping members.
- **Bug class:** bitfield load/store width, `known_bits` wide-store, unaligned
  packed-member access.
- **Oracle:** both — vs-gcc certifies bit *layout*, olevels certifies opt stability.

### 3.3 `switch` — `switch`/`goto`/labels  *(plan rank #4; most self-contained)*
- **Shapes:** dense *and* sparse `switch` on a masked selector (drives
  `SWITCH_TABLE`/jump tables); forward `goto` over bounded, fully-initializing
  regions.
- **UB invariants:** selector masked into the case domain (or a `default` that
  also folds into `cs`); no fallthrough into uninitialized reads; goto only
  forward, never skipping an initialization that a later read needs.
- **Bug class:** jump-table codegen, switch-bounds vs. loop confusion, BFS over
  case targets — a known buggy seam.
- **Oracle:** olevels (no ABI surface).

### 3.4 `struct_byval` — struct/union by value  *(plan rank #5)*
- **Shapes:** pass and `return` small structs by value through helpers; small
  unions written-then-read on the *same* member (no type-punning).
- **UB invariants:** no cross-member union reads; every field initialized;
  by-value copies only.
- **Bug class:** AAPCS struct passing, sret copies, union layout.
- **Oracle:** vs-gcc.

### 3.5 `varargs` — `stdarg`  *(plan rank #6)*
- **Shapes:** one generated `int vsum(int n, ...)` summing `n` `int` args into `cs`.
- **UB invariants:** arg count always matches `n`; all args `int`.
- **Bug class:** `stdarg` frame layout — directly the `va-arg-*` failing class.
- **Oracle:** vs-gcc.

### 3.6 `ptr` — restricted pointers / aliasing  *(plan rank #7; biggest seam, strictest discipline)*
- **Shapes:** `&local`, in-bounds `&arr[i]`, single-level deref load/store.
- **UB invariants:** no escaping, no type-punning, no out-of-bounds, no
  use-after-scope; pointers never compared or stored.
- **Bug class:** DSE, load-CSE, store-forwarding, deref-forward, offset-0
  chained-store — a large fraction of the historical memo record.
- **Oracle:** olevels (alias-opt stability).

---

## 4. D2 — seed range (cheap; do alongside D1)

- **Certified band** (GATE): exhaustive `triage_olevels.sh 0 9999` per profile —
  must stay 0 fails. One standalone crt0 boot per seed/level (full recall).
- **Advancing frontier:** `FAST_SWEEP=1 batch_sweep.py 10000 50000` to pre-scan,
  then `triage_olevels.sh` only the flagged seeds (real bisect + repro). Grow the
  certified band by the confirmed-clean delta.
- **Caveat:** `batch_sweep` is ~80% recall — never use it to *certify* a band,
  only to *find* candidates. Reconcile the two stale root tables
  (`fuzz_triage_0_10000.md`, `fuzz_triage_2000_10000.md`) into one canonical
  `(profile, seed)` tracker as part of this.

## 5. D3 — oracle expansion (close the O0-WRONG blind spot)

- Promote **vs-gcc** from a 12-seed smoke to a **swept gate** (it already honors
  `FUZZ_VSGCC_SEEDS` and `FUZZ_PROFILE`): `FUZZ_VSGCC_SEEDS=0-9999 pytest …`.
- Wire `triage_olevels.sh` to emit an **`O0-WRONG`** row when O0 itself diverges
  from `gcc -m32 -funsigned-char`, and auto-escalate that seed to vs-gcc for
  confirmation. This makes the fuzzer the *generative* complement to the fixed
  `failing_tests.txt` torture corpus — most valuable once `float`/`varargs`
  profiles emit the exact features those O0 failures use.

## 6. D4 — optimization pressure (secondary multiplier)

- Add a few `-fno-<pass>` / `TCC_DISABLE_PASS` / `TCC_SKIP_SSA` configs as extra
  "levels" in the olevels oracle — the bisection knobs become **finders** of
  pass-ordering bugs, not just localizers.
- Add `-Os` to the vs-gcc oracle for layout-dependent bugs (literal-pool / branch
  narrowing — cf. tests 192, 207).

---

## 7. Sequencing

```
NOW   ─ harvest float backlog (3.0) ──┐
                                       ├─ in parallel, cheap & independent:
      ─ D2 certify 0–9999 + 1 canonical table
      ─ D3 promote vs-gcc to swept gate
                                       │
THEN  ─ D1 profiles, top-down, harvest between each:
        3.1 fnptr → 3.2 bitfield → 3.3 switch → 3.4 struct_byval → 3.5 varargs → 3.6 ptr
                                       │
EACH   uses §2 contract · its own fuzz_triage_<profile>_*.md · ir_tests/NN_fuzz_* harvest
ALONG  D4 knobs layered onto whichever oracle the active profile uses
```

Rationale: D2/D3 are independent of D1 and make every later profile's sweep more
trustworthy, so they go first/concurrently. D1 is strictly top-down by bug
density. D4 rides along.

## 8. Definition of done

- A canonical `(profile, seed) → class → root cause → fix commit → regression
  test` tracker, replacing the two stale root tables.
- **vs-gcc** runs as a swept gate with O0-WRONG escalation, not a smoke.
- Certified band `0–9999` green per landed profile, with an advancing frontier.
- Top-3 D1 profiles beyond `int` landed behind `--profile` (`float`✅ + fnptr +
  bitfield), each with its own swept+certified band and harvested regressions.
- Each axis has a `docs/plan_fuzz_<axis>.md` instantiating §2.3, written when that
  axis is picked up.
```
