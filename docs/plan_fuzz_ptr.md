# Plan: ptr profile — fuzz restricted single-level pointers / aliasing to break the memory optimizer

> **Profile name:** `ptr`  **Feature flag:** `"ptr"`  **Master-plan row:** [§3.6](plan_fuzz_coverage_master.md) (plan rank #7 — *biggest seam, strictest discipline*).
>
> One-line goal: emit UB-free `&local` / `&arr[i & (N-1)]` addresses and **single-level** deref loads/stores — including a deliberate **alias pair** — so the loaded/stored *data* (never the address) flows into `cs`, exercising the DSE / load-CSE / store-forwarding / deref-forward / offset-0 chained-store passes that dominate the historical bug record.

This is the **highest-risk axis for false positives** in the whole feature space: the generator currently bans pointers outright ([`gen_c.py:36`](../tests/fuzz/gen_c.py) — *"No pointer/aliasing tricks, no UB casts, no function-pointer games"*). Reintroducing them safely is the entire engineering problem. Every shape below is constrained so a divergence can *only* mean a real miscompile.

---

## 1. Authoring contract anchor

Instantiates the [§2.3 template](plan_fuzz_coverage_master.md) and obeys the [§2.1 hard invariants](plan_fuzz_coverage_master.md). The **only** file this profile touches is [`gen_c.py`](../tests/fuzz/gen_c.py); everything downstream keys off `FUZZ_PROFILE` / `--profile` already (verified: [`triage_olevels.sh:30`](../tests/fuzz/triage_olevels.sh), [`batch_sweep.py:139`](../tests/fuzz/batch_sweep.py), [`test_random_c_olevels.py:66`](../tests/fuzz/test_random_c_olevels.py), [`test_random_c_vs_gcc.py:70`](../tests/fuzz/test_random_c_vs_gcc.py)).

---

## Status — IMPLEMENTED (2026-06-30) · DENSEST bug seam OPEN

The `ptr` profile is **landed in [`gen_c.py`](../tests/fuzz/gen_c.py)** — the strictest-discipline
axis, validated address-safe and finding the most bugs of any profile:

- **M0–M4 done.** All 7 prior streams byte-identical (seeds {0,1,2,7,42,100,1000});
  `arm-none-eabi-gcc -O0/-O1/-O2/-Os -Wall -Wextra` compile-clean over seeds 0–40 (0 err / 0 warn).
  Single-level `unsigned *` at `&local` / `&arr[i&7]` (offset-0, high-elem, runtime-base targets),
  deliberate alias pairs, `deref` leaf + `ptrstore`/`aliasrw` statements, mandatory `*p` data-fold.
- **I9/I1/I2 verified structurally:** a precise per-token audit found **0 illegal pointer uses
  across 400 seeds** — every pointer occurrence is `*p` or its `&` declaration, so **no address
  can reach `cs`** (the false-positive that kills this axis). Confirmed by 4-way cross-oracle:
  the checksum is layout-independent (`host-gcc-64 == arm-gcc-O2 == tcc-O0`).
- **M5 done — DENSEST seam.** olevels pre-scan **183 divergent / 5001** (most of any profile;
  the historical 19x–21x alias band). Culprits vary: seed 22 = SSA-pipeline (no single `-fno`
  knob isolates → use `TCC_SKIP_SSA`), seed 67 = `const-prop`. Both 4-way confirmed real.
- **Harvest OPEN (M6–M8):** worklist in [`fuzz_triage_ptr_0_5000.md`](../fuzz_triage_ptr_0_5000.md).

---

## 2. Bug class unlocked

The pointer-deref / alias seam is where the **densest cluster of historical fuzz bugs** already lives — every test in the `19x`–`21x` band plus the `test_sl_fwd_alias*` and `bug_*_store*` families. Reintroducing pointers in the *generator* (not just hand-written regressions) turns this from a corpus of post-hoc repros into a continuously sampled bug source. The passes it exercises, with confirmed defs:

| Pass | Symbol & location | What the alias/deref shape stresses |
|---|---|---|
| **Store-load forwarding** | `tcc_ir_opt_sl_forward` — [`ir/opt_memory.c:1209`](../ir/opt_memory.c) | A store via `*p` then a load via `*q` where `p`,`q` may alias: forwarding the stored value across an aliasing read is the canonical miscompile (cf. `test_sl_fwd_alias.c`, 199, 208, 214). |
| **Entry-store propagation** | `tcc_ir_opt_entry_store_prop` — [`ir/opt_memory.c:198`](../ir/opt_memory.c) | A pointer write into an entry-block-initialized object that the pass thinks is still the initializer (cf. 193, 198, 204, 206, 215). |
| **Redundant-store elimination** | `tcc_ir_opt_store_redundant` — [`ir/opt_memory.c:4544`](../ir/opt_memory.c) | A `*p = a; *q = b;` chain where the pass must *not* kill the first store if a runtime deref may read it first (cf. 213, 217). |
| **DSE** | `tcc_ir_opt_dse` — [`ir/opt_dce.c:2794`](../ir/opt_dce.c) | Dead-store decisions that hinge on whether a later deref reads the slot (cf. 178). |
| **Load-CSE** | `ssa_opt_load_cse` — [`ir/opt/ssa_opt_load_cse.c:1292`](../ir/opt/ssa_opt_load_cse.c); `tcc_ir_opt_ptr_load_cse` — [`ir/opt_memory.c:10594`](../ir/opt_memory.c) | Two `*p` reads with a possibly-aliasing intervening `*q` store: CSE-ing the second read to the first is wrong if they alias (cf. 211). |
| **Deref-forward / runtime-base alias resolution** | `rse_resolve_temp_addr` [`ir/opt_memory.c:4386`](../ir/opt_memory.c), `rse_resolve_runtime_base` [`ir/opt_memory.c:4405`](../ir/opt_memory.c) | A deref through a TEMP holding `base + runtime_index` that the exact-offset resolver bails on (cf. 217 — the *densest* memo). |
| **Offset-0 chained store** | LEA fold path; `gaddrof` LEA emit [`tccgen.c:2974`](../tccgen.c) | `(p)->f = C` at member offset 0 losing the DEREF marker (cf. `bug_chained_assign_store_off0.c`). |

The opcodes all this rides on: `TCCIR_OP_LOAD`/`STORE`/`LEA` ([`tccir.h:68`–`71`](../tccir.h)) and `LOAD_INDEXED`/`STORE_INDEXED` ([`tccir.h:74`–`75`](../tccir.h)); `&x` lowers via `gaddrof` → `TCCIR_OP_LEA` ([`tccgen.c:2974`](../tccgen.c) for `&local`, [`tccgen.c:24504`](../tccgen.c) for indexed `&arr[i]`); deref load/store materialize at [`ir/codegen.c:3013`](../ir/codegen.c) (`case TCCIR_OP_LOAD`) and [`ir/codegen.c:3136`](../ir/codegen.c) (`case TCCIR_OP_STORE`).

---

## 3. UB-freedom invariants — the strict discipline

This axis lives or dies on these rules. Each is enforced **structurally** (the way the int profile masks shifts/indices), never by hope. **A single leak here produces a false positive whose "divergence" is just two compilers putting an object at different addresses.**

| # | Invariant | Structural enforcement |
|---|---|---|
| I1 | **Single-level deref only.** Pointer type is always `unsigned *`; never `unsigned **`, never a pointer-to-pointer. | The generator only ever forms `unsigned *p`; the only RHS that yields a pointer is `&<scalar>` / `&<array elem>`. No production produces a pointer *value* as data; `_leaf`/`expr` never return a pointer. |
| I2 | **Address never escapes.** A pointer is never returned, stored into memory (no `arr[i]=p`, no `st.f=p`), never assigned to a `uvar`, never passed to a helper. | Pointers live in a *separate* `Gen.pvars` set that is **disjoint** from `uvars`/`arrays`/`structs`. No emission path copies a `pvar` anywhere except a deref `*p`. Helpers never receive pointers (params stay `unsigned`). |
| I3 | **No use-after-scope.** A pointer never outlives its pointee. | Pointers and their pointees are both declared at the top of `main` (function-lifetime locals / arrays). `pvars` is pushed/popped with the same block discipline `uvars` uses in loops ([`gen_c.py:427`/`447`](../tests/fuzz/gen_c.py)); a pointer created inside a block is dropped at block exit so no later read can name it. Pointees are *never* block-local, so a pointer can never outlive its target. |
| I4 | **No pointer comparison / arithmetic on the pointer value.** | The only operators applied to a `pvar` are unary `*` (load and store-target). No `==`, `<`, `p+k`, `p-q`, `(unsigned)p` is ever emitted. |
| I5 | **No type-punning.** Pointer type always matches pointee type: `unsigned *` → `unsigned` object only. | `pvars` only ever point at `unsigned` scalars (`uvars`) or `unsigned[ARRAY_SIZE]` elements (`arrays`). `svars` (signed) and `fvars` (FP) are **never** addressable. No cast inserts a different pointer type. |
| I6 | **No out-of-bounds.** `&arr[i]` only for in-range `i`. | The pointee index reuses the existing in-bounds mask `& (ARRAY_SIZE-1)` via `_index_expr` ([`gen_c.py:276`](../tests/fuzz/gen_c.py)). A pointer is captured for **one fixed** masked index at creation; it is never incremented, so it cannot drift OOB. |
| I7 | **No uninitialized pointee read.** Every object a pointer can target is fully initialized before any pointer exists. | Pointers are declared *after* all `uvars`/`arrays` are initialized ([`gen_c.py:582`–`595`](../tests/fuzz/gen_c.py)). A deref **store** `*p = e` writes a defined `unsigned`; a deref **load** reads an object that was either an initializer or a prior defined store. |
| I8 | **Aliasing is well-defined, not undefined.** The deliberate alias pair points two `unsigned *` at the *same* `unsigned` object — legal aliasing, identical result on every conforming compiler. | The alias pair is built by taking `&x` twice (or `&arr[k]` for the *same constant* `k`) into `p` and `q`; both have type `unsigned *` (no strict-aliasing violation). |
| I9 | **Output-sensitive but address-blind.** Only loaded/stored *data* enters `cs`; pointer *values* never do. | `cs = csmix(cs, *p)` folds the **pointee value**. There is **no** path that does `csmix(cs, (unsigned)p)`. Final `cs` folding (the tail loop at [`gen_c.py:622`](../tests/fuzz/gen_c.py)) already iterates `uvars`/`arrays`, capturing every store's effect; `pvars` are *not* in that loop. |

> **Why I9 is the crux of this axis (vs. float):** the float profile could fold raw bits because bit patterns are deterministic. A pointer's bit pattern (the *address*) is **not** deterministic across tcc vs gcc, nor even across O-levels (stack layout shifts). Folding an address would make *every* seed "diverge." So the contract is absolute: addresses are write-only inputs to `*`, never values.

---

## 4. Emission shapes

All masking/casting matches the existing int-profile idioms so the new C is as warning-clean as the rest. `N == ARRAY_SIZE == 8`, so `& (N-1)` is `& 7u`.

**(a) Pointer to a scalar local — store then checksum the read-back:**
```c
unsigned *p = &u3;                 /* I5: unsigned* -> unsigned local      */
*p = (unsigned)(<expr>);           /* deref STORE  (TCCIR_OP_STORE)        */
cs = csmix(cs, *p);                /* I9: DATA, never (unsigned)p          */
```

**(b) Pointer to an in-bounds array element — load and store via deref:**
```c
unsigned *q = &arr6[(957377377u) & 7u];   /* I6: fixed masked index        */
cs = csmix(cs, *q);                        /* deref LOAD  (codegen.c:3013)  */
*q = (unsigned)(<expr>);                   /* deref STORE                   */
```

**(c) Deliberate ALIAS pair — the store-forwarding / DSE / load-CSE trigger:**
```c
unsigned *p = &u3;                 /* p and q may point at the SAME object */
unsigned *q = &u3;                 /*   (I8: both unsigned*, legal alias)  */
*p = (unsigned)(<exprA>);          /* store via p ...                      */
cs = csmix(cs, *q);                /* ... load via q: MUST see exprA       */
*q = (unsigned)(<exprB>);          /* store via q ...                      */
cs = csmix(cs, *p);                /* ... load via p: MUST see exprB       */
```
If `sl_forward` / `store_redundant` / `load_cse` wrongly assume `p` and `q` don't alias, one of the two `csmix` reads picks up the stale value and `cs` diverges from `tcc -O0`. The array variant points `p = &arr6[k]; q = &arr6[k]` at the same constant `k`, and a runtime-index variant points `q = &arr6[i & 7u]` to hit `rse_resolve_runtime_base` ([`ir/opt_memory.c:4405`](../ir/opt_memory.c)) — the exact seam of test 217.

**(d) Offset-0 chained-store variant** (targets `bug_chained_assign_store_off0.c`'s pass): point a pointer at the *first* element `&arr6[0]` (offset 0) and store through it, then read the array tail-fold — checks the DEREF marker survives the `base+0` collapse.

In every shape: the pointer is a write-only handle into `*`; only the dereferenced **unsigned** flows to `cs`.

---

## 5. gen_c.py implementation sketch — exact edit points

All new emission gated on `self.has("ptr")` so when the flag is absent **no rng value is drawn and no text is emitted** (mirror the `has("float")` discipline at [`gen_c.py:353`](../tests/fuzz/gen_c.py), [`607`](../tests/fuzz/gen_c.py)). Verify byte-identity afterward (see §10).

**Edit 1 — register the profile** ([`gen_c.py:89`](../tests/fuzz/gen_c.py), `PROFILES`):
```python
PROFILES = {
    "int":   frozenset(),
    "float": frozenset({"float"}),
    "ptr":   frozenset({"ptr"}),        # NEW — single-level deref + aliasing
}
```

**Edit 2 — new Gen state** (`Gen.__init__`, near [`gen_c.py:136`](../tests/fuzz/gen_c.py), alongside `fvars`):
```python
# (name, pointee_expr, ctype) live, in-scope unsigned* pointers ("ptr" profile).
# pointee_expr is the EXACT lvalue text the pointer aliases (e.g. "u3" or
# "arr6[(957377377u) & 7u]") so we know what *p reads/writes.  DISJOINT from
# uvars/arrays/structs (I2: a pointer is never a value, never escapes).
self.pvars: list[tuple[str, str]] = []   # (ptr_name, lvalue_text)
```

**Edit 3 — declare pointers AFTER all pointees are initialized** (in `generate_program`, immediately after the arrays/structs init block, before FP locals — i.e. after [`gen_c.py:603`](../tests/fuzz/gen_c.py)). Guarantees I7 (pointee already initialized) and I3 (function-lifetime pointee):
```python
if g.has("ptr"):
    # Build pointers to already-initialized unsigned scalars / array elements.
    targets = list(g.uvars)                         # &scalar
    targets += [f"{a}[{ARRAY_SIZE-1}u]" for a in g.arrays]  # fixed in-bounds elem
    if targets:
        n_ptr = g.rng.randint(1, 3)
        for _ in range(n_ptr):
            tgt = g.rng.choice(targets)
            name = g.fresh("p")
            main.append(f"  unsigned *{name} = &{tgt};")   # gaddrof -> LEA
            g.pvars.append((name, tgt))
        # Deliberate ALIAS pair: a SECOND pointer to a target already aliased.
        if g.rng.random() < 0.6 and g.pvars:
            base = g.rng.choice(g.pvars)
            name = g.fresh("p")
            main.append(f"  unsigned *{name} = &{base[1]};")  # same pointee
            g.pvars.append((name, base[1]))
```
> Targets use a **constant** array index here so the pointer is provably in-bounds and stable (I6). A *runtime*-index alias variant (to reach `rse_resolve_runtime_base`) can be added behind the same flag using `_index_expr()` for the pointee text — still in-bounds by I6's existing mask.

**Edit 4 — gated `_leaf` "deref" read** (in `_leaf`, [`gen_c.py:246`](../tests/fuzz/gen_c.py)): add the kind only when pointers exist, so a deref read can appear anywhere an unsigned operand is wanted (feeds load-CSE):
```python
if self.has("ptr") and self.pvars:
    choices.append("deref")
...
if kind == "deref":
    name, _ = self.rng.choice(self.pvars)
    return f"(*{name})"          # I9: returns DATA; pointer never a value
```

**Edit 5 — gated statements** (in `statement`, after the float block at [`gen_c.py:353`](../tests/fuzz/gen_c.py)):
```python
if self.has("ptr") and self.pvars:
    opts += ["ptrstore", "aliasrw"]
...
if kind == "ptrstore":                       # deref STORE then checksum read-back
    name, _ = self.rng.choice(self.pvars)
    return [f"{pad}*{name} = (unsigned)({self.expr(MAX_EXPR_DEPTH)});",
            f"{pad}cs = csmix(cs, *{name});"]
if kind == "aliasrw":                         # store via one, load via another
    if len(self.pvars) >= 2:
        p, q = self.rng.sample(self.pvars, 2)
    else:
        p = q = self.pvars[0]
    return [f"{pad}*{p[0]} = (unsigned)({self.expr(MAX_EXPR_DEPTH)});",
            f"{pad}cs = csmix(cs, *{q[0]});",
            f"{pad}*{q[0]} = (unsigned)({self.expr(MAX_EXPR_DEPTH)});",
            f"{pad}cs = csmix(cs, *{p[0]});"]
```

**Edit 6 — scope discipline so a pointer never outlives its pointee** (in the loop body of `statement`, [`gen_c.py:427`](../tests/fuzz/gen_c.py)/[`447`](../tests/fuzz/gen_c.py)): pointers are created only at the top of `main` over function-lifetime pointees, so `pvars` need no per-loop save/restore for *escape*. But if a future variant ever creates a pointer **inside** a block, mirror the existing `saved = list(self.uvars) ... self.uvars = saved` pattern with `self.pvars` so the in-block pointer is dropped at block exit (I3). For the sketch above, pointers stay top-level and this is a no-op guard documented in a comment.

**Edit 7 — keep pointers OUT of the final fold loop** ([`gen_c.py:622`](../tests/fuzz/gen_c.py)): do **nothing** — `pvars` is deliberately absent from the tail `for v in g.uvars` loop. The pointees (`uvars`/`arrays`) are already folded there, so every store's *effect* on memory reaches `cs`, while the pointer's *address* never does (I9).

**Edit 8 — prologue:** no change needed (`#include <stdio.h>` already present; no helper like `fbits_*` is required because we fold plain `unsigned`). Leaving `_prologue` ([`gen_c.py:457`](../tests/fuzz/gen_c.py)) untouched keeps the int stream byte-identical.

---

## 6. Oracle

**Primary: olevels** ([`test_random_c_olevels.py`](../tests/fuzz/test_random_c_olevels.py)) — `tcc -O0` (assumed correct) vs `-O1/-O2/-Os`. **Justification:** this axis targets *alias-opt stability*. Every pass in §2 (`sl_forward`, `store_redundant`, `entry_store_prop`, `dse`, `load_cse`) runs at `-O1`/`-O2` and is **off at `-O0`**, so a wrong alias decision shows up precisely as an O0-vs-O1/O2 divergence — exactly what the historical 19x–21x bugs were. There is **no new ABI surface** (no struct-by-value, no varargs), so olevels is sufficient and has full recall (one crt0 boot per seed/level).

**Secondary cross-check: vs-gcc** ([`test_random_c_vs_gcc.py`](../tests/fuzz/test_random_c_vs_gcc.py), `arm-none-eabi-gcc -O2` gold) — catches the **O0-WRONG** sub-class (a deref miscompiled identically at *all* tcc levels, e.g. the offset-0 DEREF-marker drop of `bug_chained_assign_store_off0.c`, which a pure olevels oracle is blind to). Run as a confirmatory pass on any flagged seed and as a periodic swept smoke.

---

## 7. Sweep & certify

```bash
# Exhaustive, authoritative triage (full recall — the band that certifies).
FUZZ_PROFILE=ptr tests/fuzz/triage_olevels.sh 0 999
#   -> writes fuzz_triage_ptr_0_999.md with class / culprit-knob / repro per seed.

# Fast pre-scan for the advancing frontier (NOT for certification).
python3 tests/fuzz/batch_sweep.py 0 5000 --profile ptr
```

> **Recall caveat for THIS axis.** `batch_sweep.py` is ~80% recall and the master plan ([§0](plan_fuzz_coverage_master.md), [§4](plan_fuzz_coverage_master.md)) names exactly the *context-sensitive uninit-read class* as what it under-recalls — and the alias/deref bug is structurally that class (whether a deref reads a slot a store was elided from). **So lean on the exhaustive `triage_olevels.sh` for any band you certify; use `batch_sweep.py` only to find candidates on the frontier**, never to declare a band clean.

```bash
# pytest wrappers (both honor FUZZ_PROFILE):
FUZZ_PROFILE=ptr FUZZ_OLEVEL_SEEDS=0-999 pytest -s tests/fuzz/test_random_c_olevels.py
FUZZ_PROFILE=ptr FUZZ_VSGCC_SEEDS=0-999  pytest -s tests/fuzz/test_random_c_vs_gcc.py
```

**Certify:** exhaustive `0–N` green on olevels (primary) with the flagged seeds also green on vs-gcc, before advancing the frontier. Reconcile findings into the canonical `(profile, seed)` tracker the master plan calls for.

---

## 8. Harvest

For each distinct root cause, add a regression test continuing the sequence (the **highest existing is `219_fuzz_strd_spill_dryrun_offset.c`**, so start at **`220`**), plus a fuzz memory:

- `tests/ir_tests/220_fuzz_<cause>.c` + `tests/ir_tests/220_fuzz_<cause>.expect`, registered in `TEST_FILES` of [`tests/ir_tests/test_qemu.py`](../tests/ir_tests/test_qemu.py). Reduce the diverging generated program to a minimal hand-written repro in the style of [`217_fuzz_store_redundant_runtime_deref_alias.c`](../tests/ir_tests/217_fuzz_store_redundant_runtime_deref_alias.c) — a top comment naming the culprit pass + the `-fno-<pass>` knob that "fixes" it + the `gcc -m32 -funsigned-char` ground-truth checksum.
- A fuzz memory per root cause (same convention as the float/19x harvest — the per-root-cause memo recording seed, culprit knob from `bisect_opt.py`, the misfolded IR line, and the fix commit) so the next session inherits the diagnosis.

Likely landing spots given the seam: a new `220+` in the `sl_forward` / `store_redundant` / `load_cse` family, or a deref-marker fix adjacent to `bug_chained_assign_store_off0.c`.

---

## 9. Feature-specific false-positive traps

This section is the crux of the axis — each trap turns a "miscompile" into a meaningless address/UB disagreement, so the generator must close it *structurally*:

| Trap | Why it's a false positive | Generator defense |
|---|---|---|
| **Address in output** | tcc and gcc place objects at different addresses; folding `(unsigned)p` makes *every* seed diverge. | I9: pointers are write-only handles into `*`. No production emits `(unsigned)p` or folds a pointer value; only `*p` (data) reaches `cs`. `pvars` excluded from the tail fold loop (Edit 7). |
| **Uninitialized pointee read** | A deref load of an uninitialized object is UB → legal disagreement. | I7: pointers declared only *after* all `uvars`/`arrays` are initialized (Edit 3, placed after [`gen_c.py:603`](../tests/fuzz/gen_c.py)); a deref store always writes a defined `unsigned` first when it's the only writer. |
| **Dangling / escaped pointer** | Returning/storing a pointer, or one outliving its pointee, is UB. | I2 + I3: `pvars` disjoint from all value sets; nothing copies a `pvar` except `*p`; pointees are function-lifetime; in-block pointers (if ever added) popped at block exit like `uvars`. |
| **Type-punned deref** | `unsigned*` aliasing a `float`/`signed`/`struct` violates strict aliasing → UB. | I5: `pvars` only target `unsigned` scalars and `unsigned[]` elements; `svars`/`fvars` are never addressable; no reinterpreting cast. |
| **OOB index** | `&arr[i]` with `i ∉ [0,N)` then deref is UB. | I6: pointee index reuses the proven `& (ARRAY_SIZE-1)` mask ([`_index_expr`, gen_c.py:276](../tests/fuzz/gen_c.py)); the pointer captures one fixed masked index and is never incremented. |
| **Pointer comparison / arithmetic** | `p < q`, `p - q`, `p == q` on unrelated objects is UB or non-portable. | I4: the only operator applied to a `pvar` is unary `*`. No relational/arith/equality operator ever names a `pvar`. |
| **Multi-level deref** | `**pp` widens the UB surface (pointer-to-pointer init, aliasing). | I1: only `unsigned *` is ever formed; no production yields a pointer *value*, so `&p` / `unsigned **` can never arise. |
| **"Legal" alias that isn't** | Aliasing two *different-typed* pointers at one object is UB even if the math works. | I8: the alias pair is two `unsigned *` at the **same** `unsigned` object — well-defined aliasing, identical on every conforming compiler; the only thing that may differ is a *buggy* tcc optimization. |

---

## 10. Milestones

- [x] **M0 — Byte-identity gate.** Add `PROFILES["ptr"]` (Edit 1) and all `has("ptr")` gates. Prove the default stream is unchanged: `for s in 0 1 2 5 42 100 765 1205; do diff <(python3 tests/fuzz/gen_c.py --seed $s) <(git stash; python3 tests/fuzz/gen_c.py --seed $s; git stash pop); done` (or compare against `--profile int` explicitly) → **zero diff**. No rng draw, no text when flag absent.
- [x] **M1 — Pointer declarations (Edit 2, 3).** `python3 tests/fuzz/gen_c.py --profile ptr --seed N` emits `unsigned *p = &<initialized target>;` after pointees are initialized. Manual read confirms I5/I6/I7.
- [x] **M2 — Deref read + store (Edit 4, 5).** `deref` leaf + `ptrstore` statement emit `*p` reads/writes; only data reaches `cs`. Eyeball 10 seeds for I9 (no `(unsigned)p` anywhere).
- [x] **M3 — Alias pair (Edit 3 alias clause + `aliasrw`).** Two pointers at one object, store-via-one/load-via-other present. Confirm const-index and runtime-index variants both appear.
- [x] **M4 — Compiles clean.** `tcc -O0/-O1/-O2/-Os -c` and `gcc -m32 -funsigned-char -w -c` both succeed on seeds 0–200, no warnings beyond the int profile's.
- [x] **M5 — First sweep.** `FUZZ_PROFILE=ptr tests/fuzz/triage_olevels.sh 0 999`; record divergences in `fuzz_triage_ptr_0_999.md`. Expect early hits in the `sl_forward`/`store_redundant`/`load_cse` family (this seam historically diverges fast).
- [ ] **M6 — Triage + bisect.** For each cluster, `bisect_opt.py` to the culprit knob and the misfolded IR line; confirm `gcc` ground truth = `tcc -O0`.
- [ ] **M7 — Harvest.** Minimal `220+_fuzz_*.c` (+`.expect`) per root cause in `tests/ir_tests/`, registered in `test_qemu.py`; one fuzz memory each. Fix the underlying pass; regression goes green.
- [ ] **M8 — Certify band.** Exhaustive olevels `0–N` green (primary) with flagged seeds also vs-gcc green; advance the frontier with `batch_sweep.py` as *finder only*; reconcile into the canonical `(profile, seed)` tracker.
