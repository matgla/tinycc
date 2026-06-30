# Plan: fnptr profile — fuzz indirect calls through a dispatch table of DAG helpers

> Instantiates §2.3 of [`plan_fuzz_coverage_master.md`](plan_fuzz_coverage_master.md)
> for row [§3.1](plan_fuzz_coverage_master.md). **Profile name:** `fnptr`.
> **Feature flag:** `"fnptr"`. Purely additive to [`gen_c.py`](../tests/fuzz/gen_c.py);
> the default `int` stream stays byte-identical.

```
## Profile: fnptr             feature flag: "fnptr"
UB invariants     : exact-prototype targets · index masked [0,N) · table fully init · no ptr escape/compare
Emission shapes   : static unsigned (*tab[N])(unsigned,unsigned); tab[idx&(N-1)](a,b) → cs
Bug class unlocked: indirect-call ABI, sret-through-fnptr, stack-arg delivery, fn-ptr CSE/devirt
Oracle            : vs-gcc (ABI-shaped) + olevels for opt stability
Sweep             : FUZZ_PROFILE=fnptr tests/fuzz/triage_olevels.sh 0 999
                    python3 tests/fuzz/batch_sweep.py 0 5000 --profile fnptr
Certify           : exhaustive 0–N green vs-gcc + olevels
Harvest           : ir_tests/NN_fuzz_<cause>.c (+.expect) per root cause, + memory
```

---

## Status — IMPLEMENTED (2026-06-30)

The `fnptr` profile is **landed in [`gen_c.py`](../tests/fuzz/gen_c.py)** and validated:

- **M0–M4 done.** `int` + `float` streams byte-identical for seeds {0,1,2,7,42,100};
  `arm-none-eabi-gcc -O1 -Wall -Wextra` compile-clean over fnptr seeds 0–40 (0 err / 0 warn);
  dispatch table, `icall` leaf, `icall_cs` statement, `_index_expr_n`, and the
  set-but-unused guard all emit as specified (≈75% of seeds roll ≥1 helper and get a table).
- **M5–M6 done — first band CLEAN.** olevels pre-scan (`batch_sweep`) **0 divergent over
  seeds 0–5000**; vs-gcc gold gate **1000/1000 pass over seeds 0–999** (gcc-O2 == tcc-O0/O1/O2).
- **No root cause to harvest yet** (M7/M8 open): the `unsigned(unsigned,unsigned)` target shape
  deliberately avoids the **sret + stack-arg** seam (the standing workaround at
  [arm-thumb-gen.c:363](../arm-thumb-gen.c#L363)). **Recommended follow-up:** a sibling shape
  with **64-bit-returning** table targets (§9) to drive sret-through-fnptr directly — the most
  likely place a real bug hides.

---

## 1. Goal

Make the fuzzer emit **indirect calls through a function-pointer table** so the
differential oracle exercises the entire indirect-call lowering path — argument
register/stack placement, the pre-save of a fn-pointer that landed in an arg
register, and fn-pointer CSE — none of which the `int`/`float` profiles ever
reach. The dispatch targets are the **DAG helpers already emitted** by
`_emit_helper` (all `unsigned(unsigned,unsigned)`), so the table is trivially
type-correct and the call graph stays a strict DAG.

---

## 2. Bug class unlocked

Indirect calls take a different backend path from direct `BL` calls, and that
path has *already* produced shipped bugs in this fork. Concrete seams:

- **Indirect-call lowering / target in an arg register.** The call site lowers a
  `MACH_OP_REG` target to `BLX` and must pre-save the pointer when the register
  allocator placed it in `R0–R3` (those get clobbered by argument setup). See
  the pre-save block in
  [`tcc_gen_machine_func_call_mop` (arm-thumb-gen.c:11852)](../arm-thumb-gen.c#L11852)
  and the dispatch comment for `MACH_OP_REG → BLX`
  [arm-thumb-gen.c:10255](../arm-thumb-gen.c#L10255) inside
  [`gcall_or_jump_mop` (arm-thumb-gen.c:10261)](../arm-thumb-gen.c#L10261).
  `func_call_mop` itself: [arm-thumb-gen.c:11763](../arm-thumb-gen.c#L11763).
- **Fn-pointer clobbered as a stack arg.** Reduced from musl `qsort`: a comparator
  passed as the 5th arg (stack) was loaded into `r1`, then `r1` was reused for
  the 2nd call arg before the `BLX` — see
  [`tests/ir_tests/bug_funcptr_fifth_arg.c`](../tests/ir_tests/bug_funcptr_fifth_arg.c).
  Our table-call form `tab[idx&(N-1)](a,b)` puts the *pointer* through a register
  while `a,b` go to `r0,r1`, the exact collision pattern.
- **sret-through-fnptr + stack args.** There is a standing workaround that routes
  certain indirect calls through a *direct* call because the cross-compiler
  miscompiles indirect calls combining an **sret return with stack-passed
  arguments** (callee reads garbage for the 5th/6th params): see
  [`thumb_call_imm_handler` comment (arm-thumb-gen.c:363)](../arm-thumb-gen.c#L363)
  and the `reg_handler` twin at
  [arm-thumb-gen.c:4785](../arm-thumb-gen.c#L4785) /
  [`thumb_call_reg_handler` (arm-thumb-gen.c:4791)](../arm-thumb-gen.c#L4791).
  Our `unsigned(unsigned,unsigned)` targets do **not** use sret, but a follow-up
  shape (§9, table of 64-bit-returning helpers) would probe that seam directly.
- **Call ABI encoding.** `FUNCCALLVAL` carries `(call_id, argc)` packed in
  `src2.c.i` via [`TCCIR_ENCODE_CALL` (tccir.h:245)](../tccir.h#L245); the
  indirect target is the call's `src1` operand. The opcode is
  [`TCCIR_OP_FUNCCALLVAL` (tccir.h:67)](../tccir.h#L67); computed-target jumps use
  [`TCCIR_OP_IJUMP` (tccir.h:61)](../tccir.h#L61). The frontend emits
  `FUNCCALLVAL` at [tccgen.c:4086](../tccgen.c#L4086),
  [tccgen.c:7530](../tccgen.c#L7530), [tccgen.c:7573](../tccgen.c#L7573), pulling
  the target from `vtop[-1]`/`vtop[-2]` — for an indirect call that SValue is a
  loaded pointer rather than a `VT_SYM`, so the same op flows down both paths.
- **AAPCS arg placement.** Register/stack assignment for the two `unsigned` args
  is [`tcc_abi_classify_argument` (arch/arm/arm_aapcs.c:26)](../arch/arm/arm_aapcs.c#L26);
  the scalar-in-register / spill-to-stack logic the table call stresses begins at
  [arch/arm/arm_aapcs.c:67](../arch/arm/arm_aapcs.c#L67).
- **Fn-ptr type handling.** `VT_FUNC`/pointer-to-function decay in
  [tccgen.c](../tccgen.c) — e.g. the `VT_FUNC` ref-copy at
  [tccgen.c:2660](../tccgen.c#L2660) and `bt == VT_FUNC` cast/store paths at
  [tccgen.c:5395](../tccgen.c#L5395), [tccgen.c:11105](../tccgen.c#L11105).
- **Devirtualization / CSE.** Because all table slots are compile-time-constant
  addresses of known helpers, an over-eager optimizer could devirtualize
  `tab[idx&(N-1)]` to a wrong helper, or CSE two distinct `tab[i]`/`tab[j]` loads
  — caught by olevels since the result is in `cs`.

Prior-art tests to mirror in shape:
[`bench_indirect_calls.c`](../tests/ir_tests/bench_indirect_calls.c) (table of
`func_ptr_t ops[4]`, `ops[n&3](value)`),
[`30_function_call.c`](../tests/ir_tests/30_function_call.c),
[`98_call_over32_args.c`](../tests/ir_tests/98_call_over32_args.c),
[`nested_funcptr_indirect.c`](../tests/ir_tests/nested_funcptr_indirect.c),
[`bug_funcptr_fifth_arg.c`](../tests/ir_tests/bug_funcptr_fifth_arg.c).

---

## 3. UB-freedom invariants (the authoring contract, restated + enforced)

1. **Default stream untouched.** Every emission below is gated on
   `g.has("fnptr")`. When the flag is absent, **no `rng` value is drawn and no
   text is produced** — mirroring exactly how the `float` profile gates `expr`/
   `_leaf`/`statement`/`_prologue`/`generate_program` on `has("float")`. Prove
   byte-identity: `diff <(gen_c.py --profile int --seed S) <(checkout-before …)`
   for a handful of seeds S after the change.
2. **Exact-prototype targets (no ABI UB).** All dispatch targets are the
   generator's own helpers, *all* of which are `static unsigned NAME(unsigned
   pa, unsigned pb)` (signature emitted by [`_emit_helper` (gen_c.py:533)](../tests/fuzz/gen_c.py#L533)).
   The table is typed `unsigned (*tab[N])(unsigned, unsigned)` and called with two
   `unsigned` args — calling a function through a pointer of its exact type is
   defined; no prototype mismatch is possible.
3. **Index always in `[0, N)`.** `N` is a power of two and the call index is
   masked `idx & (N - 1)` — identical to the array-index discipline in
   [`_index_expr` (gen_c.py:276)](../tests/fuzz/gen_c.py#L276). No
   out-of-bounds table read.
4. **Table fully initialized.** The table is a `static`/local aggregate whose
   every slot is a helper name, written at declaration — no uninitialized slot,
   no null entry, so every `BLX` target is a valid function.
5. **No pointer escape / compare / print.** Pointers are *never* compared,
   subtracted, stored into `cs`, printed, or otherwise observed. Only the
   `unsigned` **result** of the call flows into `cs`. (Function addresses differ
   between tcc and gcc links, so any pointer value in `cs` would be a guaranteed
   false positive — §9.)
6. **Output-sensitive.** Each indirect-call result is folded into `cs` via
   `csmix`, exactly like direct-helper results at
   [gen_c.py:627](../tests/fuzz/gen_c.py#L627).
7. **Bounded & terminating.** Targets are the existing strict-DAG helpers (no
   recursion). The table call adds no loop and no allocation; only a masked index
   and one `BLX`.

**Precondition.** The fnptr machinery only emits when **at least one helper
exists** (`g.helpers` non-empty). If `n_helpers == 0` for a seed, the fnptr
profile emits nothing extra for that seed (still UB-free, just no indirect call).

---

## 4. Emission shapes (concrete C)

A **single dispatch table** is declared in `main()` after the live vars, once,
when `has("fnptr")` and helpers exist. `N` = next pow-of-two ≥ `len(helpers)`,
slots filled round-robin from `g.helpers` so every slot is a real helper:

```c
/* fnptr profile: dispatch table over the DAG helpers (all unsigned(unsigned,unsigned)). */
static unsigned (*const dtab[4])(unsigned, unsigned) = {
    helper1, helper2, helper3, helper1   /* round-robin fill to pow2 length */
};
```

Indirect-call **leaf** (a new `_leaf` kind `"icall"`, only added to the choice
list when `has("fnptr")` and the table exists). The index is masked; the two
args are ordinary unsigned expressions; the result is a plain unsigned value that
slots into any expression context (and thus reaches `cs` like every other leaf):

```c
dtab[((unsigned)(<idx_expr>) & 3u)]((unsigned)(<expr>), (unsigned)(<expr>))
```

Indirect-call **statement** (a new `statement` kind `"icall_cs"`, gated the same
way) folds a result straight into the checksum — guarantees output-sensitivity
even when the leaf form is not sampled:

```c
cs = csmix(cs, dtab[((unsigned)(<idx_expr>) & 3u)]((unsigned)(<expr>), cs));
```

Reusing `<idx_expr>` = `_index_expr()` keeps the mask power-of-two consistent;
the table-size mask is `N - 1` (its own `N`, not `ARRAY_SIZE`).

---

## 5. gen_c.py implementation sketch (exact edit points)

All line refs below are against the current [`gen_c.py`](../tests/fuzz/gen_c.py).

**(a) Register the profile** — [PROFILES dict, gen_c.py:89](../tests/fuzz/gen_c.py#L89):
```python
PROFILES = {
    "int":   frozenset(),
    "float": frozenset({"float"}),
    "fnptr": frozenset({"fnptr"}),     # adds an indirect-call dispatch table
}
```

**(b) New Gen state** — in `Gen.__init__` near
[gen_c.py:142](../tests/fuzz/gen_c.py#L142), add the table name + size, empty
until emitted (so `has("fnptr")` alone never enables the leaf — the table must
also exist):
```python
self.dtab_name: str | None = None   # set once the dispatch table is declared
self.dtab_n: int = 0                # table length (power of two)
```
Add a small tunable near the other limits ([gen_c.py:63](../tests/fuzz/gen_c.py#L63)):
`MAX_DTAB = 8`.

**(c) Declare the table in `generate_program`** — *after* helpers are emitted and
`g.callable_helpers` is set ([gen_c.py:562](../tests/fuzz/gen_c.py#L562)), and
after the unsigned-locals block so it lives inside `main()`. Place it right
before `main.append("")` at [gen_c.py:615](../tests/fuzz/gen_c.py#L615):
```python
if g.has("fnptr") and g.helpers:
    n = 1
    while n < min(len(g.helpers), MAX_DTAB):
        n <<= 1                      # smallest pow2 >= len(helpers), capped
    n = max(n, 1)
    slots = [g.helpers[i % len(g.helpers)] for i in range(n)]
    name = g.fresh("dtab")
    main.append(f"  static unsigned (*const {name}[{n}])(unsigned, unsigned)"
                f" = {{ {', '.join(slots)} }};")
    g.dtab_name, g.dtab_n = name, n
```
*Gating discipline:* the **entire** `if` (including every `g.rng`-free line) is
inside `if g.has("fnptr")`, so the `int` stream draws nothing new. `g.fresh` only
bumps a counter (no rng), so even name allocation cannot perturb the `int`/`float`
streams. `static const` keeps the pointer values immutable (helps the optimizer
see them and stresses devirt/CSE) and avoids any runtime table init.

**(d) Gated leaf kind `"icall"`** — in `_leaf`
([gen_c.py:246](../tests/fuzz/gen_c.py#L246)), append the choice *only* when the
table exists, then handle it. Mirror the `float`-profile pattern of conditionally
extending the `choices` list:
```python
if self.has("fnptr") and self.dtab_name:
    choices.append("icall")
...
if kind == "icall":
    idx = self._index_expr_n(self.dtab_n)         # masked into [0, dtab_n)
    a = self.expr_for_icall()                     # see note
    b = self.expr_for_icall()
    return f"{self.dtab_name}[{idx}]({a}, {b})"
```
Add a tiny helper `_index_expr_n(self, n)` next to
[`_index_expr` (gen_c.py:276)](../tests/fuzz/gen_c.py#L276) that masks with
`n - 1` instead of `ARRAY_SIZE - 1`. **Depth/termination note:** `_leaf` has no
depth param, so to avoid unbounded nesting (`icall` args drawing more `icall`s)
the args `a,b` must be **shallow** — generate them with `self._leaf()` *minus the
icall option*, or pass a guard flag. Simplest: a `self._icall_depth` guard
(default 0); when entering `"icall"` set it so nested `_leaf` calls skip the
`"icall"` choice; restore after. This keeps each indirect call's args to plain
leaves/exprs and bounds total calls per program.

**(e) Gated statement kind `"icall_cs"`** — in `statement`
([gen_c.py:344](../tests/fuzz/gen_c.py#L344)), extend `opts` exactly like the
`float` block at [gen_c.py:353](../tests/fuzz/gen_c.py#L353):
```python
if self.has("fnptr") and self.dtab_name:
    opts += ["icall_cs", "icall_cs"]
...
if kind == "icall_cs":
    idx = self._index_expr_n(self.dtab_n)
    arg = self.expr(MAX_EXPR_DEPTH)
    return [f"{pad}cs = csmix(cs, {self.dtab_name}[{idx}]"
            f"((unsigned)({arg}), cs));"]
```

**(f) Prologue** — **no change needed**. Unlike `float` (which adds
`#include <string.h>` and `fbits_*` at [gen_c.py:457](../tests/fuzz/gen_c.py#L457)),
fnptr needs no extra includes or helpers; the table reuses existing helpers and
`csmix`. Confirm `_prologue` stays byte-identical for `fnptr` *when no table is
emitted* and identical-modulo-the-table otherwise.

**(g) Unused-helper safety.** `generate_program` already calls *every* helper
directly at [gen_c.py:626](../tests/fuzz/gen_c.py#L626), so helpers referenced
only via the table never warn `-Wunused-function`. The table itself is "used"
because it is read by every `icall`/`icall_cs`; if a seed declares the table but
samples neither (possible), add one mandatory fold after the body — guard with
`if g.dtab_name:` — e.g.
`cs = csmix(cs, dtab[0](1u, cs));` so the table is never set-but-unused.

---

## 6. Oracle

**Primary: vs-gcc** ([`test_random_c_vs_gcc.py`](../tests/fuzz/test_random_c_vs_gcc.py)).
Indirect calls are **ABI-shaped**: the historical bugs here
([`bug_funcptr_fifth_arg.c`](../tests/ir_tests/bug_funcptr_fifth_arg.c), the
sret+stack-arg workaround at [arm-thumb-gen.c:363](../arm-thumb-gen.c#L363)) are
all cases where *every* tcc opt level agrees but the argument/return ABI is
wrong — exactly the **O0-WRONG** blind spot olevels cannot see (master plan
§0/§5). `arm-none-eabi-gcc -O2` is the gold ABI reference, so vs-gcc is the
certifying gate.

**Secondary: olevels** ([`test_random_c_olevels.py`](../tests/fuzz/test_random_c_olevels.py))
still adds value: it certifies **opt stability** of the fn-ptr path —
devirtualization, CSE of two `dtab[i]` loads, and the pre-save register choice
([arm-thumb-gen.c:11852](../arm-thumb-gen.c#L11852)) all differ across O0/O1/O2
and would diverge there. Run both; vs-gcc gates, olevels finds the cheaper
opt-ordering bugs (D4 knobs ride on olevels).

---

## 7. Sweep & certify (exact commands)

```bash
# Exhaustive, authoritative O-level triage (real bisect + repro per flagged seed):
FUZZ_PROFILE=fnptr tests/fuzz/triage_olevels.sh 0 999
#   → writes fuzz_triage_fnptr_0_999.md  (plumbing: triage_olevels.sh:30 keys off FUZZ_PROFILE)

# Fast pre-scan only (~200 seeds/boot, ~80% recall — NEVER certify with this, only find):
python3 tests/fuzz/batch_sweep.py 0 5000 --profile fnptr
#   (batch_sweep.py:139/332 honor --profile / FUZZ_PROFILE)

# Both pytest wrappers under the fnptr profile:
FUZZ_PROFILE=fnptr pytest -s tests/fuzz/test_random_c_olevels.py
FUZZ_PROFILE=fnptr FUZZ_VSGCC_SEEDS=0-999 pytest -s tests/fuzz/test_random_c_vs_gcc.py

# Inspect one generated program:
python3 tests/fuzz/gen_c.py --profile fnptr --seed 42
```

**Certify:** exhaustive `0–N` **green on vs-gcc** (the ABI gate) *and* clean
olevels, growing the certified band per master-plan §4 (`batch_sweep` only to
*find* candidates, `triage_olevels.sh` to certify).

---

## 8. Harvest

Per confirmed root cause, add a regression test continuing the sequence — the
**highest existing is `219_*`** (verified: `ls tests/ir_tests | grep -oE '^[0-9]+'
| sort -n | tail` → 219). Numbers are assigned **at harvest time** (next free is
`220_fuzz_<cause>.c`), each with its `.expect` and registered in `TEST_FILES`
in [`tests/ir_tests/test_qemu.py`](../tests/ir_tests/test_qemu.py):

```
tests/ir_tests/220_fuzz_<cause>.c        # minimized repro from gen_c.py --profile fnptr --seed S
tests/ir_tests/220_fuzz_<cause>.expect   # gold output (from gcc reference / tcc -O0 if O0 correct)
```

Plus a **fuzz memory** per root cause (the `docs/fuzz_*` / triage-table entry
recording `(profile=fnptr, seed) → class → root cause → fix commit → regression
test`), following the cadence in
[`fuzz_triage_guide.md`](fuzz_triage_guide.md) and root-causing with
[`debugging_fuzz_divergences.md`](debugging_fuzz_divergences.md) /
`scripts/bisect_opt.py`.

---

## 9. Feature-specific false-positive traps (and how the generator avoids each)

| Trap | Why it would be a false positive | Generator defense |
|---|---|---|
| **Pointer value in output** | Function addresses differ between the tcc link and the gcc link (different `_start`/section layout); a printed/checksummed address diverges legitimately. | Pointers are *never* observed — only the `unsigned` **call result** enters `cs`. No `&fn`, no ptr compare, no ptr in `printf`. (§3.5) |
| **Prototype mismatch** | Calling a fn through a pointer of the wrong type is UB → conforming compilers may legally disagree. | *Every* target is the generator's own `unsigned(unsigned,unsigned)` helper ([gen_c.py:533](../tests/fuzz/gen_c.py#L533)); the table type is exactly that; calls pass exactly two `unsigned`. No mismatch is constructible. |
| **Out-of-bounds table index** | Reading past the table → indeterminate target. | Index masked `& (N-1)`, `N` a power of two ([`_index_expr_n`]). |
| **Uninitialized / null slot** | A null `BLX` target is a trap, not a miscompile. | Table fully initialized round-robin from `g.helpers`; the precondition requires ≥1 helper. |
| **Indirect-vs-direct convention drift** | The frontend may emit the *same* helper as a direct `BL` (line [tccgen.c:626 fold](../tests/fuzz/gen_c.py#L626)) and an indirect `BLX` (table); if the two used different conventions only one would be right, but that is a *real* bug we WANT — not a false positive. The trap is masking it: don't let the workaround at [arm-thumb-gen.c:363](../arm-thumb-gen.c#L363) silently route the indirect call through a direct call and hide the divergence. | Keep targets sret-free (`unsigned` return), so the sret+stack-arg direct-call workaround does NOT trigger and the genuine `BLX` path is exercised. A later mini-plan can add a 64-bit-returning table specifically to probe that workaround. |
| **Address-ordering / CSE legality** | `static const` table lets the optimizer fold/CSE slot loads; if a real divergence appears it is genuine devirt/CSE, but a *non-deterministic* slot order would be noise. | Slot order is deterministic from `g.helpers` (RNG-seeded, reproducible); same seed → identical table on tcc and gcc. |

---

## 10. Milestones (ordered checklist)

- [x] **M0 — byte-identity guard.** Before any edit, snapshot `gen_c.py --profile
      int --seed {0,1,2,7,42,100}` outputs. After every change, re-diff: must be
      byte-identical. (Same proof the `float` profile passed.)
- [x] **M1 — register profile.** Add `"fnptr"` to `PROFILES`
      ([gen_c.py:89](../tests/fuzz/gen_c.py#L89)); add `dtab_name`/`dtab_n` state +
      `MAX_DTAB`. Verify `--profile fnptr` runs and `int` diff is clean (M0).
- [x] **M2 — table declaration.** Emit the `static const` table in
      `generate_program` after helpers ([gen_c.py:615](../tests/fuzz/gen_c.py#L615)),
      gated on `has("fnptr") and g.helpers`. Inspect `--profile fnptr --seed N`:
      table present, every slot a real helper, length a power of two.
- [x] **M3 — leaf + statement.** Add gated `"icall"` leaf (with the
      `_icall_depth` recursion guard) and `"icall_cs"` statement; add
      `_index_expr_n`. Confirm result flows into `cs` and the program still has no
      ptr escape.
- [x] **M4 — compile both oracles.** `--profile fnptr --seed {0..20}` compiles
      clean under tcc **and** `arm-none-eabi-gcc` with no new warnings (esp.
      `-Wunused-function`, `-Wint-conversion`); table never set-but-unused (§5g).
- [x] **M5 — sweep.** `FUZZ_PROFILE=fnptr tests/fuzz/triage_olevels.sh 0 999` and
      `batch_sweep.py 0 5000 --profile fnptr`; collect candidate seeds.
- [x] **M6 — vs-gcc gate.** `FUZZ_PROFILE=fnptr FUZZ_VSGCC_SEEDS=0-999 pytest …
      test_random_c_vs_gcc.py`; triage every divergence with `bisect_opt.py`.
- [ ] **M7 — harvest.** Minimize each root cause → `ir_tests/220_fuzz_*.c`
      (+`.expect`, register in `test_qemu.py`); write a fuzz memory per cause; fix;
      re-certify the band green on both oracles.
- [ ] **M8 — certify & advance.** Exhaustive `0–N` green (vs-gcc + olevels);
      record the certified band; advance the frontier per master-plan §4.
