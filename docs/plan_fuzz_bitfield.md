# Plan: `bitfield` profile — fuzz bitfield/packed-struct load & store codegen

> **Profile name:** `bitfield`  **Feature flag:** `"bitfield"`
> **Goal:** additively teach [`gen_c.py`](../tests/fuzz/gen_c.py) to emit UB-free
> structs with **unsigned** bitfields (mixed widths) and a `#pragma pack(1)` /
> `__attribute__((packed))` variant, fold each *named field's* value into the
> rolling checksum, and thereby fuzz the bitfield insert/extract/RMW codegen
> seam that the `int`/`float` profiles never touch.
>
> This is §3.2 of [`plan_fuzz_coverage_master.md`](plan_fuzz_coverage_master.md)
> and instantiates the §2.3 targeted-plan template. It obeys the §2.1 hard
> invariants verbatim (restated in §3 below).

---

## 1. Profile summary (§2.3 template)

```
## Profile: bitfield          feature flag: "bitfield"
UB invariants     : unsigned bitfields only; every field value masked to its
                    bit-width before store; every field initialised before read;
                    read NAMED fields only (never the struct as raw bytes).
Emission shapes   : a non-packed bitfield struct (mixed widths :1/:3/:7/:13) and a
                    packed variant (#pragma pack(1) and/or __attribute__((packed)));
                    field writes `f = value & ((1u<<N)-1)`; field reads `cs = csmix(cs, s.f)`.
Bug class unlocked: bitfield load/store width, known_bits wide-store, unaligned
                    packed-member access, RMW insert/extract fold correctness.
Oracle            : BOTH — vs-gcc certifies bit LAYOUT, olevels certifies opt stability.
Sweep             : FUZZ_PROFILE=bitfield tests/fuzz/triage_olevels.sh 0 999
                    python3 tests/fuzz/batch_sweep.py 0 5000 --profile bitfield
Certify           : exhaustive 0–N green on both oracles
Harvest           : ir_tests/NN_fuzz_<cause>.c (+.expect) per root cause (NN ≥ 219), + memory
```

---

## Status — IMPLEMENTED (2026-06-30) · bug seam OPEN

The `bitfield` profile is **landed in [`gen_c.py`](../tests/fuzz/gen_c.py)** and is already
finding real bugs:

- **M0–M3 done.** `int`+`float`+`fnptr` streams byte-identical (seeds {0,1,2,7,42,100});
  `arm-none-eabi-gcc -O0/-O1/-O2 -Wall -Wextra` compile-clean over seeds 0–40 (0 err / 0 warn);
  non-packed `struct BF` + packed `struct BFP` (`#pragma pack(1)` + `__attribute__((packed))`),
  masked `bfstore` writes, named-field-only folds — all gated, no raw-byte reads.
- **M4 done — RICH bug seam.** olevels pre-scan **162 divergent / 5001**; vs-gcc gold gate
  **30 divergent / 1000**. Confirmed real (not generator UB) by 5-way cross-oracle on seed 5:
  `arm-gcc-O2 == tcc-O0 == tcc-O1 == host-gcc-64 == 669df7f1`, only `tcc-O2 = d1c27a84` wrong.
- **Diagnosis.** vs-gcc set ⊆ olevels set and tcc-O0 is correct ⇒ these are **`-O1/-O2` optimizer
  miscompiles, not layout/O0-WRONG bugs** (tcc bitfield layout matches gcc). Seed-5 culprit knobs
  (QEMU-confirmed): **`store-load-fwd` / `const-prop` / `loop-unroll`** — store→load forwarding of a
  bitfield RMW store (the packed-RMW class, cf. test 184).
- **Harvest OPEN (M5–M7):** worklist in [`fuzz_triage_bitfield_0_5000.md`](../fuzz_triage_bitfield_0_5000.md).

---

## 2. Bug class unlocked

Bitfields and packed members exercise codegen seams that *no* value computed by
the `int`/`float` profiles can reach, because the relevant C is never emitted.
Each store/read of a bitfield lowers to a shift+mask insert or a two-shift
extract, and a packed/unaligned field falls into an entirely separate
byte-at-a-time path. Concretely, the seams this profile drives are:

- **Bitfield load / extract width.** Reading a field that is *not* at bit 0
  lowers (in the aligned case) to the two-shift sign/zero-extension form
  `(word << (bits-(pos+size))) >> (bits-size)` emitted at
  [`tccgen.c:3296-3302`](../tccgen.c#L3296). Getting the shift amounts or the
  signedness of the second shift wrong drops high bits or reads the wrong
  window. The IR-level fold that must recognise and collapse the insert→extract
  round-trip lives in `ir/opt_bitfield.c` (the
  `tcc_ir_opt_bitfield_insert_extract` pass guarded by prior-art test
  [`174_bitfield_extract_fold.c`](../tests/ir_tests/174_bitfield_extract_fold.c)).

- **Bitfield store / insert + `known_bits` wide-store.** A field write is a
  read-modify-write: clear the field's window with `~(mask<<pos)` and OR in
  `(V & mask) << pos` — emitted at
  [`tccgen.c:12256-12277`](../tccgen.c#L12256) (the `mask = (1ULL<<bit_size)-1`,
  `~(mask<<bit_pos)` clear). If the optimizer's known-bits tracking believes the
  field is wider/narrower than it is, the RMW clobbers an adjacent field or
  leaves stale bits. The float profile's `known_bits` machinery has never seen a
  sub-word store.

- **Unaligned packed-member access.** For packed structs (`align == 1`) a field
  that straddles a byte boundary takes the byte-at-a-time path
  `load_packed_bf` at [`tccgen.c:3158`](../tccgen.c#L3158) and
  `store_packed_bf` at [`tccgen.c:3192`](../tccgen.c#L3192), reached via the
  `r == VT_STRUCT` branch at [`tccgen.c:3287-3289`](../tccgen.c#L3287) (load) and
  [`tccgen.c:12252`](../tccgen.c#L12252) (store). The layout that selects this
  path is computed in `struct_layout` at
  [`tccgen.c:12989`](../tccgen.c#L12989) (the bitfield case begins at
  [`tccgen.c:13085`](../tccgen.c#L13085); pack/`packed` lowers `align` at
  [`tccgen.c:13047-13059`](../tccgen.c#L13047)). The packed RMW store has a
  documented historical miscompile, guarded by
  [`184_packed_bitfield_rmw_store.c`](../tests/ir_tests/184_packed_bitfield_rmw_store.c)
  (stale use-list entry corrupting the store's base-address operand at -O1).

- **Struct-copy → poke-one-field → re-read fold.** `struct y = s; y.k += x;
  return y.k;` (the `174_*` idiom) expands to an in-register insert immediately
  followed by an extract; the small-all-bitfield-word and unit-copy heuristics
  at [`tccgen.c:10741-10860`](../tccgen.c#L10741)
  (`struct_is_small_bitfield_word`, `bitfield_unit_width`,
  `ir_emit_struct_unit_copy`) decide whether the copy collapses. A random
  generator mixing widths and pack states will reach corner cases the four
  hand-written `174/184` shapes do not.

#pragma pack state is threaded through the preprocessor as a deferred replay
token (`pp_apply_pack_replay` at [`tccpp.c:1879`](../tccpp.c#L1879),
`*pack_stack_ptr` consumed in `struct_layout` at
[`tccgen.c:12994`](../tccgen.c#L12994)) — another seam (push/pop balance) we
exercise for free by emitting `#pragma pack(push,1) … #pragma pack(pop)`.

---

## 3. UB-freedom invariants

The §2.1 contract says any divergence must be a *real* miscompile, never a legal
disagreement between two conforming compilers. Bitfields add three brand-new UB /
implementation-defined hazards on top of the integer rules; we close each
**structurally** in the generator so a divergence can only mean a codegen bug.

1. **Unsigned bitfields only.** Signedness of a plain `int x:N` bitfield is
   *implementation-defined* (C11 §6.7.2.1) — tcc and gcc could legitimately
   differ on whether `int b:3 = 7` reads back as `7` or `-1`. The generator
   declares **every** bitfield as `unsigned …:N` (or `unsigned int …:N`), so the
   value domain is the unambiguous `[0, 2^N)`. **No signed bitfield is ever
   emitted.** (Mirrors the integer profile's "compute in unsigned, only read
   signed scalars" rule.)

2. **Mask to field width before every store.** The stored value is always
   `(unsigned)(expr) & ((1u << N) - 1)` where `N` is the field's width. Storing
   an over-wide value into `unsigned f:N` is *defined* (the value is taken
   modulo 2^N), but masking up-front makes the *intended* value and the
   *read-back* value provably identical, so a width-truncation codegen bug is
   detectable rather than masked by C's own truncation. For `N == 32`
   (`unsigned f:32`) the mask is `0xffffffffu` (no `1u<<32` UB — see trap §9).

3. **Every field initialised before read.** Each struct instance is brace- or
   field-initialised at declaration so no field is read uninitialised. Because we
   never `memset`/`memcpy` the struct as bytes, only declared fields exist in the
   value domain — there is no uninitialised *padding* that a read could observe.

4. **Read NAMED fields only — never raw bytes.** The checksum fold reads
   `s.fieldname` for every declared field; it **never** does
   `memcpy(&u, &s, sizeof s)` or `((unsigned char*)&s)[i]`. The bits *between*
   fields and the trailing pad bits of a packed struct are **indeterminate**
   (not required to be zero, not required to agree across compilers), so folding
   raw bytes would be a guaranteed false positive. This is the bitfield analogue
   of the float profile's care — but inverted: float *can* safely reinterpret
   bits via `memcpy` because every bit of a nominal-width FP object is defined;
   a bitfield struct's inter-field bits are *not*, so we must stay at the named-
   field abstraction. **The `fbits_*`/`memcpy` trick is forbidden for this
   profile.**

5. **Bounded & terminating.** No new loops, recursion, or allocation — field
   writes/reads are straight-line statements folded into the existing block /
   final-fold machinery. (Inherited unchanged.)

6. **Default `int` stream stays byte-identical.** Every line in §5 is gated on
   `g.has("bitfield")`. When the flag is absent **no `self.rng` value is drawn
   and no text is emitted** — verified the same way `float` is (`diff` a few
   `int` seeds before/after). New helpers in the prologue and the new struct type
   are likewise emitted only under the flag.

---

## 4. Emission shapes (concrete C)

A `bitfield`-profile program adds **two** struct types and folds each field of
each instance into `cs`. Example skeleton (widths/instances chosen by the RNG):

```c
/* non-packed: natural alignment, mixed widths -> aligned extract/insert path */
struct BF {
  unsigned a : 1;
  unsigned b : 3;
  unsigned c : 7;
  unsigned d : 13;
  unsigned e : 8;          /* total < 32 -> single storage unit */
};

/* packed variant: align 1 -> some fields straddle bytes -> load/store_packed_bf */
#pragma pack(push, 1)
struct BFP {
  unsigned p : 5;
  unsigned q : 11;         /* crosses a byte boundary under pack(1) */
  unsigned r : 6;
  unsigned s : 13;
} __attribute__((packed));  /* belt-and-suspenders: pragma AND attribute */
#pragma pack(pop)

int main(void) {
  unsigned cs = 0x12345678u;
  /* ...existing int locals... */

  struct BF  bf  = { 0u, 0u, 0u, 0u, 0u };   /* all fields initialised */
  struct BFP bfp = { 0u, 0u, 0u, 0u };

  /* field writes: value masked to the field's width N before store */
  bf.c  = (unsigned)(<expr>) & ((1u << 7) - 1);     /* 7-bit field  */
  bf.d  = (unsigned)(<expr>) & ((1u << 13) - 1);    /* 13-bit field */
  bfp.q = (unsigned)(<expr>) & ((1u << 11) - 1);    /* packed RMW   */
  ++bf.b;                                            /* optional RMW shape, then re-mask via store helper */

  /* ...existing body statements... */

  /* final fold: read each NAMED field into cs (never raw bytes) */
  cs = csmix(cs, bf.a);  cs = csmix(cs, bf.b);  cs = csmix(cs, bf.c);
  cs = csmix(cs, bf.d);  cs = csmix(cs, bf.e);
  cs = csmix(cs, bfp.p); cs = csmix(cs, bfp.q);
  cs = csmix(cs, bfp.r); cs = csmix(cs, bfp.s);

  printf("checksum=%08x\n", cs);
  return 0;
}
```

Notes that keep it UB-free *and* output-sensitive:

- Field values come straight from `self.expr(MAX_EXPR_DEPTH)` (the existing
  unsigned-only expression generator) `& ((1u<<N)-1)`. A `:32` field uses the
  literal mask `0xffffffffu` (no `1u<<32`).
- Each field is read into `cs` exactly once in the final fold, so any wrong
  read/insert width changes the printed checksum.
- The packed struct mixes a width that straddles a byte (`:11`, `:13`) to force
  the `load_packed_bf`/`store_packed_bf` byte path, and includes the redundant
  `__attribute__((packed))` so both the `#pragma pack` path
  ([`tccgen.c:13051`](../tccgen.c#L13051)) and the attribute path
  ([`tccgen.c:13047`](../tccgen.c#L13047)) are exercised across seeds.

---

## 5. `gen_c.py` implementation sketch (exact edit points)

All edits gated on `g.has("bitfield")`; with the flag absent, **zero** rng draws
and **zero** new text. Pattern mirrors the existing `"float"` wiring.

### 5.1 PROFILES entry (line ~89)

```python
PROFILES = {
    "int":      frozenset(),
    "float":    frozenset({"float"}),
    "bitfield": frozenset({"bitfield"}),   # NEW: bitfields & packed structs
}
```

### 5.2 Profile tunables (near the `--- "float" profile tunables ---` block, ~line 95)

```python
# --- "bitfield" profile tunables ---------------------------------------------
# UNSIGNED fields only; widths chosen so a struct's fields sum to <= 32 bits
# (one storage unit) for the non-packed shape, and so the packed shape has at
# least one field straddling a byte boundary.  Field VALUES are always masked to
# `& ((1u<<N)-1)` before store (N==32 -> 0xffffffffu, no 1u<<32 UB).
BF_WIDTHS = [1, 2, 3, 4, 5, 6, 7, 8, 11, 13]   # all < 32; no signed surprises
BF_MIN_FIELDS, BF_MAX_FIELDS = 3, 5
```

### 5.3 New `Gen` state (in `Gen.__init__`, near `self.structs`/`self.fvars`, ~line 136)

```python
# Bitfield struct instances in scope ("bitfield" profile).  Each entry is
# (instance_name, type_name, [(field_name, width), ...]).
self.bfvars: list[tuple[str, str, list[tuple[str, int]]]] = []
```

### 5.4 Helper to pick a field set (new method on `Gen`, gated by caller)

```python
def _bf_fields(self) -> list[tuple[str, int]]:
    """A list of (field_name, width) for one bitfield struct; widths < 32,
    sum <= 32 for the non-packed shape.  All UNSIGNED."""
    n = self.rng.randint(BF_MIN_FIELDS, BF_MAX_FIELDS)
    fields, total = [], 0
    for i in range(n):
        w = self.rng.choice(BF_WIDTHS)
        if total + w > 32:            # keep non-packed struct in one unit
            break
        fields.append((f"b{i}", w))
        total += w
    if not fields:                    # guarantee at least one field
        fields = [("b0", 1)]
    return fields

def _bf_mask(self, width: int) -> str:
    return "0xffffffffu" if width >= 32 else f"((1u << {width}) - 1u)"
```

### 5.5 Struct-type emission in `generate_program` (gated; ~after the `struct S` block, line 566)

Emit the type *declarations* into `out` (top level, like `struct S`), gated:

```python
if g.has("bitfield"):
    # non-packed type
    f_np = g._bf_fields()
    np_decl = "\n".join(f"  unsigned {nm} : {w};" for nm, w in f_np)
    out.append(f"struct BF {{\n{np_decl}\n}};")
    # packed type (pragma + attribute) -- forces load/store_packed_bf path
    f_pk = g._bf_fields()
    pk_decl = "\n".join(f"  unsigned {nm} : {w};" for nm, w in f_pk)
    out.append(
        "#pragma pack(push, 1)\n"
        f"struct BFP {{\n{pk_decl}\n}} __attribute__((packed));\n"
        "#pragma pack(pop)"
    )
    g._bf_types = [("struct BF", f_np), ("struct BFP", f_pk)]
```

(Store `g._bf_types` on the instance, or just rebuild from `bfvars`; either is fine.)

### 5.6 Instance declarations in `main` (gated; near the struct-instance block, ~line 599)

```python
if g.has("bitfield"):
    n_bf = g.rng.randint(1, 2)
    for _ in range(n_bf):
        tyname, fields = g.rng.choice(g._bf_types)
        name = g.fresh("bf")
        inits = ", ".join("0u" for _ in fields)   # every field initialised
        main.append(f"  {tyname} {name} = {{ {inits} }};")
        g.bfvars.append((name, tyname, fields))
```

### 5.7 Gated store/read statements in `statement()` (~line 344)

Add a `bfstore` option to `opts` only when fields exist, and a handler:

```python
if self.has("bitfield") and self.bfvars:
    opts.append("bfstore")
...
if kind == "bfstore":
    name, _ty, fields = self.rng.choice(self.bfvars)
    fname, w = self.rng.choice(fields)
    rhs = self.expr(MAX_EXPR_DEPTH)
    return [f"{pad}{name}.{fname} = (unsigned)({rhs}) & {self._bf_mask(w)};"]
```

Optionally also emit a masked RMW shape (`++name.fname` then re-store masked, or
`name.fname = (name.fname + (small & mask)) & mask`) to drive the insert/extract
fold — but **only** when it stays in `[0,2^N)` after the mask (it always does,
because the final `& mask` re-narrows). Keep it simple first; add RMW once the
straight-store sweep is green.

### 5.8 Final fold loop in `generate_program` (gated; after the existing struct fold, ~line 635)

```python
# Fold each NAMED bitfield member into cs (NEVER raw bytes -- inter-field /
# pad bits are indeterminate and would be a false positive).
for name, _ty, fields in g.bfvars:
    for fname, _w in fields:
        main.append(f"  cs = csmix(cs, {name}.{fname});")
```

### 5.9 Prologue (line 457) — **no change needed**

Unlike `float`, this profile needs **no** new prologue helper (no `<string.h>`,
no `fbits_*`). `_prologue(seed, features)` stays byte-identical for the `int`
stream and is unchanged for `bitfield`. (Do **not** add a `memcpy` helper — §3.4.)

---

## 6. Oracle — **both** (vs-gcc + olevels)

The §3.2 row mandates both, for complementary reasons:

- **vs-gcc certifies bit LAYOUT.** olevels alone is blind to a bug where *all*
  tcc opt levels agree on a wrong field offset/width. tcc's default bitfield
  layout is **PCC/GCC-compatible**: `int pcc = !tcc_state->ms_bitfields` at
  [`tccgen.c:12993`](../tccgen.c#L12993), and `ms_bitfields` is off by default.
  So for the standard *and documented-pack* declarations this profile emits,
  tcc and `arm-none-eabi-gcc` **agree on layout by construction** — same
  field offsets, same storage units, same `#pragma pack(1)`/`packed` semantics
  (GNU pack semantics, which both implement). Any vs-gcc checksum divergence is
  therefore a *real* layout/access miscompile, not a legal ABI disagreement.
  This catches the O0-WRONG class (front-end extract/insert width bugs) that
  olevels structurally cannot see.

- **olevels certifies opt stability.** The packed RMW store bug guarded by
  [`184_packed_bitfield_rmw_store.c`](../tests/ir_tests/184_packed_bitfield_rmw_store.c)
  is an *optimizer* bug: O0 correct, O1 faults/wrong. That is exactly the
  O0=baseline self-consistency oracle's home turf, and it runs cheaper / with
  full recall per seed. Run it as the primary sweep gate; escalate divergent
  seeds to vs-gcc to distinguish O0-WRONG (layout/front-end) from O1/O2-WRONG
  (opt).

Both wrappers already key off `FUZZ_PROFILE`
([`test_random_c_olevels.py:66`](../tests/fuzz/test_random_c_olevels.py#L66),
[`test_random_c_vs_gcc.py:70`](../tests/fuzz/test_random_c_vs_gcc.py#L70)) — no
edits required.

---

## 7. Sweep & certify (exact commands)

```bash
# 1) Authoritative O-level triage of the first band (exhaustive, full recall,
#    bisects culprit knob per failing seed -> fuzz_triage_bitfield_0_999.md):
FUZZ_PROFILE=bitfield tests/fuzz/triage_olevels.sh 0 999

# 2) Fast pre-scan of a wider band to FIND candidates (~80% recall, ~200/boot;
#    never used to CERTIFY -- only to surface seeds to triage):
python3 tests/fuzz/batch_sweep.py 0 5000 --profile bitfield
#    (equivalently: FUZZ_PROFILE=bitfield python3 tests/fuzz/batch_sweep.py 0 5000)

# 3) pytest wrappers (both oracles), per docs/plan_fuzz_coverage_master.md §2.2:
FUZZ_PROFILE=bitfield FUZZ_OLEVEL_SEEDS=0-999 \
    pytest -s tests/fuzz/test_random_c_olevels.py
FUZZ_PROFILE=bitfield FUZZ_VSGCC_SEEDS=0-999 \
    pytest -s tests/fuzz/test_random_c_vs_gcc.py

# 4) Inspect / minimise one program while debugging:
python3 tests/fuzz/gen_c.py --profile bitfield --seed <N>
```

**Certify:** the band `0–N` is certified only when `triage_olevels.sh 0 N`
(exhaustive) **and** the vs-gcc swept gate over `0–N` are both 0-fail. Use
`batch_sweep` only to advance the frontier (pre-scan, then triage the flagged
seeds), exactly as D2 in the master plan prescribes — never to certify.

---

## 8. Harvest

For each root cause found, continue the regression sequence in
[`tests/ir_tests/`](../tests/ir_tests/) (current highest is **219**, so start at
**219**):

```
 tests/ir_tests/220_fuzz_<cause>.c        # minimised reproducer
 tests/ir_tests/220_fuzz_<cause>.expect   # golden checksum= line
```

- Add the new test to `TEST_FILES` in
  [`tests/ir_tests/test_qemu.py`](../tests/ir_tests/test_qemu.py) (per CLAUDE.md
  "Adding Tests").
- Minimise the generated program down to the offending struct + field
  read/write, modelled on the existing
  [`174_bitfield_extract_fold.c`](../tests/ir_tests/174_bitfield_extract_fold.c)
  (named-field reference comparison) and
  [`184_packed_bitfield_rmw_store.c`](../tests/ir_tests/184_packed_bitfield_rmw_store.c)
  (packed RMW + golden byte/checksum) shapes.
- Write a **fuzz memory** per *root cause* (not per seed) recording: profile
  `bitfield`, seed, the culprit knob from `triage_olevels.sh`, the IR seam, and
  the fix commit — following `docs/fuzz_triage_guide.md` and the existing
  `fuzz_triage_*.md` memos.

---

## 9. Feature-specific false-positive traps (and how the generator avoids each)

| Trap | Why it would be a false positive | Generator defence |
|---|---|---|
| **Signed-vs-unsigned bitfield.** `int b:3` sign of read-back is implementation-defined. | tcc and gcc may *legally* disagree (`7` vs `-1`). | Emit **`unsigned` only** (§3.1) — value domain is unambiguously `[0,2^N)`. Never emit `int`/`signed`/plain-`char` bitfields. |
| **Padding / indeterminate bits.** Bits between fields and trailing pad of a packed struct are indeterminate. | A raw-byte read could differ between compilers with neither wrong. | **Read named fields only** (§3.4); never `memcpy`/`(unsigned char*)` the struct. The `fbits_*` float trick is *banned* here. |
| **Pack-semantics mismatch.** MS vs GCC bitfield layout differ. | If tcc used MS layout it could disagree with gcc on offsets. | tcc default is PCC/GCC layout (`pcc = !ms_bitfields`, [`tccgen.c:12993`](../tccgen.c#L12993)); we never pass `-mms-bitfields`, and only use standard `#pragma pack(1)` + GNU `__attribute__((packed))`, which both compilers implement identically. |
| **Over-wide stored value.** Storing `>2^N` into `:N` is defined-but-truncated; an unmasked compare would read back the truncated value. | The *program* would still be self-consistent, but a width-truncation codegen bug could hide behind C's own truncation. | Mask the RHS to the field width **before** store (§3.2), so intended == read-back and a truncation bug is visible. |
| **`1u << 32` UB in the mask itself.** For a `:32` field, `(1u<<32)-1` is UB. | The *generator's own mask expression* would be UB, not the compiled program — but still a self-inflicted false positive. | `_bf_mask(width)` returns the literal `0xffffffffu` for `width >= 32`; widths are drawn from `BF_WIDTHS` (all `< 32`) anyway, so `:32` cannot arise. |
| **Uninitialised field read.** Reading a never-written field. | tcc/gcc could differ on the indeterminate value. | Every instance is brace-initialised to `0u` per field at declaration (§3.3); the final fold reads only declared fields. |

---

## 10. Milestones (ordered checklist)

- [x] **M0 — byte-identity guard.** Add `PROFILES["bitfield"]`, tunables, `Gen`
      state, but with **all** emission gated on `has("bitfield")`. Prove the
      `int` stream is unchanged: `python3 tests/fuzz/gen_c.py --seed K` (K in a
      few values) byte-identical before/after for the `int` profile.
- [x] **M1 — type + instance emission.** Implement `_bf_fields`/`_bf_mask`,
      `struct BF` + packed `struct BFP` declarations, and gated instance
      declarations with all-`0u` initialisers. `gen_c.py --profile bitfield
      --seed 0` compiles clean under `armv8m-tcc` and `arm-none-eabi-gcc`
      (`-Wall`, no warnings).
- [x] **M2 — masked stores + named-field folds.** Add the `bfstore` statement
      handler (RHS masked to width) and the final named-field fold loop. Confirm
      the printed checksum is sensitive to each field (manually flip one store).
- [x] **M3 — smoke both oracles.** `FUZZ_PROFILE=bitfield` over seeds `0–11` on
      olevels and vs-gcc; resolve any generator-side UB/warning before sweeping.
- [x] **M4 — sweep band 0–999.** Run `triage_olevels.sh 0 999` (authoritative)
      and `batch_sweep.py 0 5000 --profile bitfield` (pre-scan). Triage every
      flagged seed; record `fuzz_triage_bitfield_*.md`.
- [ ] **M5 — packed RMW deepening.** Add the masked RMW store shape (§5.7) to
      drive the insert/extract fold + `load/store_packed_bf` path; re-sweep.
- [ ] **M6 — harvest.** For each root cause: minimise → `ir_tests/NN_fuzz_*.c`
      (+`.expect`, NN ≥ 219) → add to `TEST_FILES` → fuzz memory. `make test`
      green.
- [ ] **M7 — certify 0–N.** Exhaustive `triage_olevels.sh 0 N` **and** vs-gcc
      swept gate `0–N` both 0-fail; advance the certified band by the
      confirmed-clean delta. Update the master-plan §8 tracker.
