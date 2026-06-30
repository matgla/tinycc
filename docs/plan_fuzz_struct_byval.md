# Plan: `struct_byval` profile — fuzz AAPCS struct passing & struct-return (sret) by emitting helpers that take and return small structs/unions by value

> **Profile name:** `struct_byval`  **feature flag:** `"struct_byval"`
> Instantiates §3.4 of [`plan_fuzz_coverage_master.md`](plan_fuzz_coverage_master.md)
> (read [§2 authoring contract](plan_fuzz_coverage_master.md#2-the-profile-authoring-contract-the-reusable-scaffold),
> [§2.3 template](plan_fuzz_coverage_master.md#23-per-profile-targeted-plan-template),
> and the [§3.4 row](plan_fuzz_coverage_master.md#34-struct_byval--structunion-by-value--plan-rank-5)).
> The **only** file this profile touches is [`tests/fuzz/gen_c.py`](../tests/fuzz/gen_c.py); everything
> downstream keys off `FUZZ_PROFILE` / `--profile` already.

```
## Profile: struct_byval        feature flag: "struct_byval"
UB invariants     : every field initialised before read; by-value copies only;
                    unions written-then-read on the SAME member (no type-punning);
                    only NAMED fields folded into cs (never raw struct bytes).
Emission shapes   : small structs of varying size (1/4/5/8 bytes) passed to AND
                    returned-by-value from generated helpers; one small union
                    written-then-read on one member; named fields fold into cs.
Bug class unlocked: AAPCS struct passing, sret copies, reg-vs-memory return
                    boundary, union layout.
Oracle            : vs-gcc (ABI/layout surface) + olevels (opt stability).
Sweep             : FUZZ_PROFILE=struct_byval tests/fuzz/triage_olevels.sh 0 999
                    python3 tests/fuzz/batch_sweep.py 0 5000 --profile struct_byval
Certify           : exhaustive 0–N green on vs-gcc + olevels
Harvest           : ir_tests/NN_fuzz_<cause>.c (+ .expect) per root cause, + memory
```

---

## 1. Goal

Add a purely-additive generator profile that emits **structs and a union passed
to and returned from generated helper functions by value**, with each helper's
returned struct's **named fields** folded into the rolling `cs` checksum. The
struct sizes are deliberately chosen to **straddle the register-vs-sret return
boundary** so both the in-register return path and the hidden-pointer (sret) path
are exercised on every interesting seed. The default `int` stream stays
byte-identical.

---

## Status — IMPLEMENTED (2026-06-30) · bug seam OPEN (one is a CRASH)

The `struct_byval` profile is **landed in [`gen_c.py`](../tests/fuzz/gen_c.py)** and found a
crash on the first sweep:

- **M0–M3 done.** All 5 existing streams (int/float/fnptr/bitfield/switch) byte-identical
  (seeds {0,1,2,7,42,100,1000}); `arm-none-eabi-gcc -O0/-O1/-O2 -Wall -Wextra` compile-clean
  over seeds 0–40 (0 err / 0 warn). Shapes `SB1/SB4/SB5/SB8` straddle the 4-byte reg/sret
  boundary; helpers take+return structs by value (`_emit_struct_helper`); `sbcall` folds named
  return fields; `union UB` written-then-read on the same member. (Simplifications vs the plan:
  struct helpers don't nest-call each other — trivially DAG-safe — and `sbcall` builds its own
  fully-initialized param struct inline, so no separate `sbvars` are needed.)
- **M4 done — CRASH found.** olevels **2 divergent / 5001** (62, 4791); vs-gcc **1 / 1000**
  (62). Seed 62 makes **tcc-O1/-O2 HardFault** (bus fault) — confirmed real by 4-way
  cross-oracle (`host-gcc == arm-gcc == tcc-O0 == 3195cd5e`). The **sole** culprit knob is
  **`store-load-fwd`** — the same pass that breaks bitfield seed 5 and switch seed 63.
- **Harvest OPEN (M5–M8):** worklist in [`fuzz_triage_struct_byval_0_5000.md`](../fuzz_triage_struct_byval_0_5000.md).
  Seed 62 is the cleanest `store-load-fwd` reproducer (a crash isolated to one knob) —
  the highest-leverage fix in the sweep.

---

## 2. Bug class unlocked

Struct-by-value passing and struct return are an entire ABI seam that the `int`
and `float` profiles never emit. This fork's struct ABI lives across the
frontend, the AAPCS classifier, the IR param/return ops, and the Thumb backend —
each is a candidate for miscompiles, and several have **already** produced
hand-written regression tests (the fuzzer has never reached this code path).

**Return boundary — the headline seam (this fork is unusually aggressive):**
[`gfunc_sret`](../arm-thumb-gen.c#L3965) decides how a struct comes back. In this
fork the threshold is **4 bytes**, not the textbook 16:

- [`arm-thumb-gen.c:3979`](../arm-thumb-gen.c#L3979) — `else if (size > 0 && size <= 4)` → returned in **r0** (`return 1`, `ret->t = VT_INT`).
- [`arm-thumb-gen.c:3987`](../arm-thumb-gen.c#L3987) — otherwise `return 0` → returned via a **hidden sret pointer** in r0.
- [`arm-thumb-gen.c:3971`](../arm-thumb-gen.c#L3971) — hard-float HFA path (`is_hgen_float_aggr`, [`arm-thumb-gen.c:3941`](../arm-thumb-gen.c#L3941)) returns via VFP doubles; reachable later if we add float-typed struct members.

So a 1- or 4-byte struct returns in a register, while a **5- or 8-byte** struct
takes the sret path — that single boundary is where most struct-return
miscompiles hide.

**sret prologue / return lowering.** The callee receives the hidden pointer in
r0 and must store the result through it:
[`tcc_ir_params_add_hidden_sret`](../ir/core.c#L672) ([`ir/core.c:680`](../ir/core.c#L680) `if (ret_nregs == 0)`, [`ir/core.c:689`](../ir/core.c#L689) consumes the r0 PARAM vreg), and `gfunc_return` reloads it from `func_vc` ([`tccgen.c:24377`](../tccgen.c#L24377)). The matching IR return op is
[`TCCIR_OP_RETURNVALUE`](../tccir.h#L56), dispatched at
[`ir/codegen.c:3809`](../ir/codegen.c#L3809) into
[`tcc_gen_machine_return_value_mop`](../arm-thumb-gen.c#L9390) (note the 64-bit
`ret_pair` R0:R1 path at [`arm-thumb-gen.c:9398`](../arm-thumb-gen.c#L9398) for the
8-byte register case).

**Param passing — AAPCS classifier.**
[`tcc_abi_classify_argument`](../arch/arm/arm_aapcs.c#L26) is the seam for struct
arguments ([`TCC_ABI_ARG_STRUCT_BYVAL`](../tccabi.h#L22)):
- [`arm_aapcs.c:107`](../arch/arm/arm_aapcs.c#L107) — composites `> 16` bytes pass by **invisible reference** (`TCC_ABI_ARG_FLAG_INVISIBLE_REF`, [`tccabi.h:70`](../tccabi.h#L70)).
- [`arm_aapcs.c:129`](../arch/arm/arm_aapcs.c#L129) — 8-byte-aligned structs trigger the **even-register** rule (`if (align >= 8 && (next_reg & 1)) next_reg++`) — an off-by-one magnet.
- [`arm_aapcs.c:143`](../arch/arm/arm_aapcs.c#L143) — `TCC_ABI_LOC_REG_STACK`: a struct that **straddles** registers and the stack (the exact shape `bug_sret_param_layout.c` reduced).

The frontend prepares the by-value copy in
[`gfunc_param_typed`](../tccgen.c#L14553) (`> 16` → invisible ref at
[`tccgen.c:14703`](../tccgen.c#L14703); fresh stack slot + `vstore` copy at
[`tccgen.c:14722`](../tccgen.c#L14722)). On the IR side the param value is
[`TCCIR_OP_FUNCPARAMVAL`](../tccir.h#L65) (encoded via
[`TCCIR_ENCODE_PARAM`](../tccir.h#L236)), classified in
[`thumb_build_call_layout_from_ir`](../arm-thumb-callsite.c#L89) (struct detected
at [`arm-thumb-callsite.c:221`](../arm-thumb-callsite.c#L221), classified
`STRUCT_BYVAL` at [`arm-thumb-callsite.c:226`](../arm-thumb-callsite.c#L226)) and
lowered by [`tcc_ir_params_process_struct`](../ir/core.c#L830) — REG case
([`ir/core.c:890`](../ir/core.c#L890)), REG_STACK straddle
([`ir/core.c:936`](../ir/core.c#L936)), STACK case
([`ir/core.c:1013`](../ir/core.c#L1013)). Backend entry points:
[`tcc_gen_machine_func_parameter_mop`](../arm-thumb-gen.c#L13267) and
[`tcc_gen_machine_func_call_mop`](../arm-thumb-gen.c#L11763)
([`ir/codegen.c:3996`](../ir/codegen.c#L3996) /
[`ir/codegen.c:4096`](../ir/codegen.c#L4096)).

**Inline struct-return expansion.** Our helpers are `static` struct-returning
functions — exactly the source-level inline-expansion path. `auto_inline_sig_ok`
([`tccgen.c:7665`](../tccgen.c#L7665)) accepts struct returns; struct params `> 16`
bytes are rejected ([`tccgen.c:7705`](../tccgen.c#L7705)); the sret buffer is
**reused** as the inline return location at
[`tccgen.c:17336`](../tccgen.c#L17336) (`if (ret_nregs == 0) inline_ret_loc =
ret.c.i;`, [`tccgen.c:17342`](../tccgen.c#L17342)). Because we call every helper
both inline-eligibly and through the normal sret path, a divergence between the
inlined copy and the called copy is directly observable in `cs` (`TCC_LOG_INLINE_STRUCT`
traces it — [`log.h:93`](../log.h#L93)).

**Union layout.** [`struct_layout`](../tccgen.c#L12989) lays union members all at
offset 0 with size = max member ([`tccgen.c:13065`](../tccgen.c#L13065)
`offset = 0; if (size > c) c = size;`), versus the accumulating struct path
([`tccgen.c:13073`](../tccgen.c#L13073)). Writing then reading the same member
exercises union slot allocation and member-offset folding without any
type-punning UB.

**Known live bug seam.** The backend already carries a workaround for an
sret-specific miscompile: indirect calls that **combine an sret return with
stack-passed arguments** read garbage for the 5th/6th parameters
([`arm-thumb-gen.c:363`](../arm-thumb-gen.c#L363), and the data-processing variant
at [`arm-thumb-gen.c:4788`](../arm-thumb-gen.c#L4788)). The fuzzer reaching this
shape with many register/stack-boundary permutations is high-value.

**Prior-art tests (confirm the seam is real and under-covered):**
[`tests/ir_tests/test_struct_pass_by_value.c`](../tests/ir_tests/test_struct_pass_by_value.c)
(Pair-by-value, mixed int/struct arg sequences),
[`tests/ir_tests/test_struct_return.c`](../tests/ir_tests/test_struct_return.c)
(Pair 8B returned-in-pair vs Big 16B via sret),
[`tests/ir_tests/bug_sret_param_layout.c`](../tests/ir_tests/bug_sret_param_layout.c)
(8B struct return + a param that lands on the stack — the REG_STACK straddle bug),
[`tests/ir_tests/bug_union_field_read.c`](../tests/ir_tests/bug_union_field_read.c)
(union member access),
[`tests/ir_tests/nested_struct_return.c`](../tests/ir_tests/nested_struct_return.c).
All hand-written — none generated by the fuzzer.

---

## 3. UB-freedom invariants (the §2.1 contract, restated + structurally enforced)

1. **Default `int` stream stays byte-identical.** Every new emission is gated on
   `g.has("struct_byval")`. When the flag is absent **no rng value is drawn and no
   text is emitted** — mirrors how `"float"` gates *all* its emission
   ([`gen_c.py:353`](../tests/fuzz/gen_c.py#L353), [`gen_c.py:607`](../tests/fuzz/gen_c.py#L607)).
   No new rng draw may sit on the unconditional path. *Prove it:* diff a handful of
   `--profile int` seeds before/after the change — they must be byte-identical.
2. **UB-free by construction.**
   - **Every field initialised before read.** Each generated struct/union is
     emitted with a full brace-initialiser of `rconst()` values (like the
     existing struct decl at [`gen_c.py:601-602`](../tests/fuzz/gen_c.py#L601)) before
     it is ever passed or read. Helpers build their returned struct from a fully
     braced initialiser of their (already-initialised) params.
   - **By-value copies only.** No `&struct`, no pointers, no escaping addresses.
     Passing and returning are plain value copies; the language guarantees a copy.
   - **Unions: written-then-read on the SAME member only.** The generator emits a
     union, writes member `m`, then reads member `m` — never a different member
     (no type-punning, the one union UB that matters here).
   - **No recursion.** Struct helpers obey the same strict-DAG rule as
     [`_emit_helper`](../tests/fuzz/gen_c.py#L491): a struct helper may only call
     **earlier** struct helpers (appended to its callable set only after its body
     is emitted) → terminating, no stack overflow.
3. **Output-sensitive — only NAMED fields reach `cs`.** Every returned struct's
   fields are folded individually (`cs = csmix(cs, ret.f0); cs = csmix(cs, ret.f1);
   …`), exactly like the existing struct fold at
   [`gen_c.py:633-635`](../tests/fuzz/gen_c.py#L633). The union folds **only the
   one member that was written**. **Never `memcpy`/read a struct as raw bytes** —
   padding between fields is indeterminate and would be a false positive (see §9).
4. **Bounded & terminating.** Bounded helper count (`MAX_STRUCT_HELPERS`), bounded
   field count, no loops introduced by this profile beyond the existing bounded
   machinery; QEMU runs stay short and deterministic.

---

## 4. Emission shapes (concrete C)

A small **fixed catalogue of struct shapes** is declared once at file scope,
chosen to straddle every ABI path: 1 byte (reg return), 4 bytes (reg return, the
boundary), 5 bytes (sret, odd size + trailing pad), 8 bytes (sret, even-register
candidate). All members are `unsigned`-family integer types so every value folds
into `cs` directly (no FP bit-reinterpret needed). The shapes:

```c
/* emitted ONLY under the struct_byval profile, at file scope */
struct SB1 { unsigned char  a; };                              /* 1 byte  -> reg return */
struct SB4 { unsigned       a; };                              /* 4 bytes -> reg return (boundary) */
struct SB5 { unsigned       a; unsigned char b; };             /* 8 bytes incl pad -> sret  */
struct SB8 { unsigned       a; unsigned      b; };             /* 8 bytes -> sret + even-reg rule */
union  UB  { unsigned       w; unsigned char b; };             /* union: write-then-read SAME member */
```

> Note: keep member types small/explicit and read fields by **name**; `SB5`'s
> trailing padding is *never* read, so its indeterminate bytes can never reach `cs`.

**A struct-returning, struct-taking helper** (analogue of
[`_emit_helper`](../tests/fuzz/gen_c.py#L491), but the prototype is
`struct SBk f(struct SBj p, unsigned x)`):

```c
static struct SB8 sbhelper3(struct SB8 p, unsigned x)
{
  /* params fully initialised by the caller; build the return value by value */
  struct SB8 r = { (unsigned)(p.a ^ (x * 3u)), (unsigned)(p.b + x) };
  /* a few safe ALU statements over r.a / r.b / p.a / p.b (all unsigned) ... */
  r.a = (unsigned)(/* g.expr(3) over {p.a,p.b,x,r.a,r.b} */);
  return r;                                   /* by-value struct return -> sret path */
}
```

Helpers vary the **return shape vs the param shape** across the catalogue so the
same seed exercises reg-return (`SB1`,`SB4`) and sret (`SB5`,`SB8`) on both the
producing and consuming side. The DAG rule lets a later helper take/return a
struct and call an earlier struct helper, threading a returned struct straight
back into another struct call (the indirect-style param-delivery shapes).

**In `main()`, gated calls that fold results into `cs`:**

```c
  struct SB8 sbv0 = { 0x11111111u, 0x22222222u };   /* fully initialised */
  struct SB8 sbr  = sbhelper3(sbv0, cs);            /* by-value pass + sret return */
  cs = csmix(cs, sbr.a);                            /* NAMED field -> cs */
  cs = csmix(cs, sbr.b);                            /* NAMED field -> cs */

  /* small union: write member w, read member w (SAME member, no punning) */
  union UB ub; ub.w = (unsigned)(/* g.expr(3) */);
  cs = csmix(cs, ub.w);
```

Every emitted struct helper is also called once at the bottom of `main()` (like
[`gen_c.py:626-627`](../tests/fuzz/gen_c.py#L626)) with deterministic args so no
helper is unused (`-Wunused-function`) and the result always reaches `cs`.

---

## 5. `gen_c.py` implementation sketch (exact edit points)

All edits are additive and gated on `g.has("struct_byval")`; when the flag is
absent the byte stream is unchanged.

**(a) Register the profile** — [`PROFILES`, `gen_c.py:89`](../tests/fuzz/gen_c.py#L89):
```python
PROFILES = {
    "int":          frozenset(),
    "float":        frozenset({"float"}),
    "struct_byval": frozenset({"struct_byval"}),   # NEW — adds by-value struct/union
}
```
(`--profile` choices come from `sorted(PROFILES)` at
[`gen_c.py:657`](../tests/fuzz/gen_c.py#L657) automatically.)

**(b) Profile tunables** (near the `float` tunables,
[`gen_c.py:95-104`](../tests/fuzz/gen_c.py#L95)):
```python
# --- "struct_byval" profile tunables -------------------------------------
MAX_STRUCT_HELPERS = 3
# (shape name, C field decls, list of field reader exprs on an lvalue `X`)
SB_SHAPES = [
    ("SB1", "unsigned char a;",                 ["a"]),                # 1B -> reg ret
    ("SB4", "unsigned a;",                       ["a"]),                # 4B -> reg ret
    ("SB5", "unsigned a; unsigned char b;",      ["a", "b"]),           # ->sret (+pad)
    ("SB8", "unsigned a; unsigned b;",           ["a", "b"]),           # ->sret, even-reg
]
```

**(c) New `Gen` state** (in `Gen.__init__`, alongside
[`gen_c.py:136-142`](../tests/fuzz/gen_c.py#L136)):
```python
self.sbhelpers: list[tuple[str, str]] = []   # (helper_name, return_shape) struct helpers
self.callable_sbhelpers: list[tuple[str, str]] = []  # DAG-restricted callable set
self.sbvars: list[tuple[str, str]] = []      # (var_name, shape) struct locals in scope
```

**(d) Emit the shape type decls** — guarded, right after the existing single
`struct S` decl at [`gen_c.py:564-566`](../tests/fuzz/gen_c.py#L564):
```python
if g.has("struct_byval"):
    for name, fields, _ in SB_SHAPES:
        out.append(f"struct {name} {{ {fields} }};")
    out.append("union UB { unsigned w; unsigned char b; };")
```

**(e) `_emit_struct_helper(g, name)`** — a sibling of
[`_emit_helper`](../tests/fuzz/gen_c.py#L491). Same scope-save/restore and same
strict-DAG discipline (`g.callable_sbhelpers = list(g.sbhelpers)` before emitting
the body, append *after*). It:
- picks a param shape `pj` and a return shape `rk` from `SB_SHAPES`
  (`g.rng.choice`), so reg-return and sret-return both occur across helpers;
- shadows `g.uvars` with the param's named fields (`["p.a", "p.b", "x"]`) plus
  `lr`, so `g.expr(...)` only references **initialised** values;
- emits `struct <rk> r = { <expr>, <expr> };` then 2–4 `r.<field> = (unsigned)(g.expr(3));`
  acc statements (mirrors the helper-body loop at
  [`gen_c.py:516-523`](../tests/fuzz/gen_c.py#L516));
- may call an **earlier** struct helper from `g.callable_sbhelpers`, feeding its
  returned struct as the param to another struct call (threads sret→param);
- ends with `return r;`. Signature line: `static struct {rk} {name}(struct {pj} p, unsigned x)`.

**(f) Wire helper emission** — alongside the scalar-helper loop at
[`gen_c.py:552-558`](../tests/fuzz/gen_c.py#L552), gated:
```python
if g.has("struct_byval"):
    n_sb = g.rng.randint(1, MAX_STRUCT_HELPERS)
    for _ in range(n_sb):
        nm = g.fresh("sbh")
        ret_shape = ...                    # chosen inside _emit_struct_helper, returned
        helper_defs.append(_emit_struct_helper(g, nm))
        g.sbhelpers.append((nm, ret_shape))
```
Then `g.callable_sbhelpers = list(g.sbhelpers)` inside `main()` (mirrors
[`gen_c.py:562`](../tests/fuzz/gen_c.py#L562)).

**(g) Declare struct locals in `main()`** — guarded, near the existing struct
decls at [`gen_c.py:598-603`](../tests/fuzz/gen_c.py#L598):
```python
if g.has("struct_byval"):
    for name, fields, readers in (g.rng.sample(SB_SHAPES, k=2)):
        v = g.fresh("sbv")
        inits = ", ".join(g.rconst() for _ in readers)   # full init -> no uninit read
        main.append(f"  struct {name} {v} = {{ {inits} }};")
        g.sbvars.append((v, name))
```

**(h) New gated statement kinds** in `statement()` — extend `opts` exactly like
the float gate at [`gen_c.py:353`](../tests/fuzz/gen_c.py#L353):
```python
if self.has("struct_byval") and self.sbvars and self.callable_sbhelpers:
    opts += ["sbcall", "sbcall"]
if self.has("struct_byval"):
    opts.append("uniongate")
```
- `"sbcall"`: pick a callable struct helper and an in-scope struct var of the
  matching param shape (or build a fresh braced literal of the right shape),
  emit `struct <rk> <t> = <helper>(<arg>, <unsigned-expr>);` then fold **each named
  field** of `<t>` into `cs` via the shape's `readers` list.
- `"uniongate"`: `union UB <u>; <u>.w = (unsigned)(self.expr(3)); cs = csmix(cs, <u>.w);`
  — write-then-read the **same** member.

**(i) Final fold-in `main()`** — after the existing helper/struct folds
([`gen_c.py:622-639`](../tests/fuzz/gen_c.py#L622)), guarded: call every struct
helper once with deterministic args (so none is unused) and fold each returned
struct's named fields into `cs` (mirrors the scalar-helper fold at
[`gen_c.py:626-627`](../tests/fuzz/gen_c.py#L626)).

No change to `_prologue` is needed (no extra `#include`; `string.h`/memcpy is the
float profile's mechanism and is **deliberately not used** here — see §9). The
checksum and `csmix` are untouched.

---

## 6. Oracle

**Primary: vs-gcc** ([`tests/fuzz/test_random_c_vs_gcc.py`](../tests/fuzz/test_random_c_vs_gcc.py),
`arm-none-eabi-gcc -O2` gold). This profile's whole point is the **ABI/layout
surface** — struct register allocation, the 4-byte sret threshold, the
even-register rule, REG_STACK straddling, union member offsets, and inline-vs-sret
copy consistency. Those are exactly the places where *all* tcc opt levels can
agree yet still disagree with the platform ABI (an O0-WRONG class). gcc-for-ARM is
the authority on AAPCS, so vs-gcc is the oracle that can catch a systematically
wrong layout. (The harness already builds gcc with the same
`-mcpu=cortex-m33 -mthumb -mfloat-abi=soft` ABI and the same boot/link recipe —
[`fuzz_harness.py:64-65`](../tests/fuzz/fuzz_harness.py#L64).)

**Secondary: olevels** ([`tests/fuzz/test_random_c_olevels.py`](../tests/fuzz/test_random_c_olevels.py)).
Struct copies, sret buffers, and the inline-struct-return expansion
([`tccgen.c:17336`](../tccgen.c#L17336)) are heavily touched by SROA / dead-store /
copy-prop / const-agg-fold; olevels certifies that O1/O2 stay consistent with the
O0 baseline (catches opt-introduced struct-copy bugs even when the ABI itself is
right). Run **both**; they cover orthogonal failure modes.

---

## 7. Sweep & certify

Everything keys off `FUZZ_PROFILE` / `--profile` already
([`triage_olevels.sh:30`](../tests/fuzz/triage_olevels.sh#L30),
[`batch_sweep.py:139`](../tests/fuzz/batch_sweep.py#L139) and `--profile` at
[`batch_sweep.py:332`](../tests/fuzz/batch_sweep.py#L332),
[`test_random_c_olevels.py:66`](../tests/fuzz/test_random_c_olevels.py#L66),
[`test_random_c_vs_gcc.py:70`](../tests/fuzz/test_random_c_vs_gcc.py#L70)).

```bash
# 0. Eyeball a few programs first (confirm shapes + reg/sret straddle):
python3 tests/fuzz/gen_c.py --profile struct_byval --seed 0
python3 tests/fuzz/gen_c.py --profile struct_byval --seed 7

# 0b. PROVE byte-identity of the default stream (must print nothing):
for s in 0 1 2 3 4 5 10 100; do
  diff <(git stash show -p 2>/dev/null; python3 tests/fuzz/gen_c.py --profile int --seed $s) \
       <(python3 tests/fuzz/gen_c.py --profile int --seed $s) ; done
# (simpler: compare against a saved pre-change copy of gen_c.py output)

# 1. Fast pre-scan (~80% recall — FINDS candidates, never certifies):
python3 tests/fuzz/batch_sweep.py 0 5000 --profile struct_byval

# 2. Authoritative triage of the flagged band (real bisect + repro, full recall):
FUZZ_PROFILE=struct_byval tests/fuzz/triage_olevels.sh 0 999
#   -> writes fuzz_triage_struct_byval_0_999.md, repros in tests/fuzz/fuzz_triage_repros/

# 3. Swept oracles (both — this profile is ABI-shaped):
FUZZ_PROFILE=struct_byval FUZZ_OLEVEL_SEEDS=0-999 pytest -s tests/fuzz/test_random_c_olevels.py
FUZZ_PROFILE=struct_byval FUZZ_VSGCC_SEEDS=0-999  pytest -s tests/fuzz/test_random_c_vs_gcc.py
```

**Certify:** a band `0–N` is certified only when **both** swept oracles are green
over the whole band via the exhaustive `triage_olevels.sh` (not `batch_sweep`,
which is ~80% recall — [`batch_sweep.py:21-33`](../tests/fuzz/batch_sweep.py#L21)).
Because struct-by-value bugs are frequently context-sensitive uninit/sret-buffer
reads — the exact class `batch_sweep` under-reports — the `triage_olevels.sh` +
vs-gcc pass is the gate. Grow the certified band by the confirmed-clean delta.

---

## 8. Harvest

For each distinct root cause, add a regression test continuing the `ir_tests`
sequence. **The highest existing number is 219**
([`tests/ir_tests/219_fuzz_strd_spill_dryrun_offset.c`](../tests/ir_tests/219_fuzz_strd_spill_dryrun_offset.c)),
so start at **220**:

1. Reduce the failing seed's program to a minimal `.c` that still diverges.
2. Save it as `tests/ir_tests/NN_fuzz_<cause>.c` (e.g.
   `220_fuzz_sret_reg_stack_straddle.c`) with a `.expect` holding the
   `checksum=<hex>` line (the gcc `-m32 -funsigned-char` / `arm-gcc -O2` gold).
3. **Register it** in `TEST_FILES` in
   [`tests/ir_tests/test_qemu.py:55`](../tests/ir_tests/test_qemu.py#L55) as
   `("NN_fuzz_<cause>.c", 0)` (append after the `219_*` entry at
   [`test_qemu.py:433`](../tests/ir_tests/test_qemu.py#L433)).
4. **Fuzz memory** = a detailed header comment in the `.c` file, in the
   established style (see
   [`218_fuzz_loop_unroll_branch_fallthrough.c`](../tests/ir_tests/218_fuzz_loop_unroll_branch_fallthrough.c)):
   `Regression: <symptom>` / `Reduced from differential-fuzz gen_c.py
   --profile struct_byval seed=<N> (<which olevels/oracle wrong>)` / `Pass:
   <pass+file>` / `Bug: <mechanism>` / `Fix: <change>` / the correct gcc checksum
   and which tcc levels were wrong. Cross-reference the matching
   `fuzz_triage_struct_byval_*.md` row.

Likely first harvests (given the seam map in §2): an sret REG_STACK straddle
mis-layout (cf. `bug_sret_param_layout.c`), an 8-byte even-register
mis-assignment ([`arm_aapcs.c:129`](../arch/arm/arm_aapcs.c#L129)), an
inline-vs-sret struct-copy divergence
([`tccgen.c:17336`](../tccgen.c#L17336)), and an opt pass dropping a struct copy
(SROA / dead-store at O1/O2).

---

## 9. Feature-specific false-positive traps (and how the generator avoids each)

1. **Padding bytes folded into `cs`.** Reading a struct as raw bytes (`memcpy` to
   an int array, like the float profile's `fbits_*`) would mix **indeterminate
   padding** (e.g. the 3 trailing pad bytes of `SB5`) into the checksum — two
   conforming compilers may leave different garbage there → false positive.
   *Avoided:* the generator **only ever folds NAMED fields** (`ret.a`, `ret.b`,
   `ub.w`) into `cs` (§3 rule 3); it never memcpys or reinterprets a struct as
   bytes. The float profile's `string.h`/memcpy mechanism is deliberately not
   pulled in.
2. **Struct size changing the ABI path mid-edit (reg vs sret).** The reg/sret
   boundary is a hard 4 bytes ([`arm-thumb-gen.c:3979`](../arm-thumb-gen.c#L3979));
   a struct whose size silently crosses it would change which path is tested
   without intent. *Avoided:* sizes come from the fixed `SB_SHAPES` catalogue with
   the path documented per shape, so every seed deterministically exercises a
   known mix of reg-return and sret. (This is a *coverage* guarantee, not a
   correctness one — both paths are valid C; we just want both reached.)
3. **Union type-punning.** Writing one member and reading another is
   implementation-defined / can be UB and would legitimately differ between
   compilers → false positive. *Avoided:* the union is **written and read on the
   SAME member** only (§3 rule 2, §4); the generator never emits a cross-member
   union read.
4. **Uninitialised struct fields / pad reads.** An uninitialised field read is UB
   and indeterminate. *Avoided:* every struct/union local is emitted with a full
   brace-initialiser of `rconst()` values before any use; helper return structs
   are built from a full braced literal of already-initialised params; only
   written members are read.
5. **Helper recursion / non-termination.** *Avoided:* struct helpers obey the same
   strict-DAG rule as the scalar helpers (callable set = strictly-earlier helpers,
   appended only after the body is emitted), so the call graph stays acyclic and
   every program terminates.
6. **Default-stream rot.** Any stray rng draw on the unconditional path would
   reshuffle every recorded seed. *Avoided:* all draws sit behind
   `g.has("struct_byval")`; the §7 byte-identity diff is the gate before landing.

---

## 10. Milestones

- [x] **M0 — Scaffold.** Add `PROFILES["struct_byval"]`
      ([`gen_c.py:89`](../tests/fuzz/gen_c.py#L89)), the `SB_SHAPES` catalogue +
      tunables, and the new `Gen` state. No emission yet.
- [x] **M1 — Byte-identity guard.** Confirm `--profile int` output is
      byte-identical to a pre-change snapshot for seeds {0,1,2,3,4,5,10,100}
      (§7 step 0b). This is the landing gate for every later milestone.
- [x] **M2 — Type decls + struct helpers.** Emit the shape/union type decls and
      `_emit_struct_helper` (DAG-restricted, reg- and sret-returning). Verify
      `--profile struct_byval --seed N` compiles clean under
      `armv8m-tcc` and `arm-none-eabi-gcc` for N in 0..50 (no warnings/errors).
- [x] **M3 — Statements + folds.** Add `sbcall` / `uniongate` statement kinds and
      the final `main()` folds; confirm every returned struct's named fields and
      the union member reach `cs`.
- [x] **M4 — Pre-scan.** `batch_sweep.py 0 5000 --profile struct_byval` runs to
      completion; collect candidate seeds.
- [ ] **M5 — Triage band 0–999.** `FUZZ_PROFILE=struct_byval triage_olevels.sh 0
      999` + both swept pytest oracles; produce
      `fuzz_triage_struct_byval_0_999.md`.
- [ ] **M6 — Harvest first root cause.** Reduce → `ir_tests/220_fuzz_<cause>.c`
      (+`.expect`), register in `TEST_FILES`
      ([`test_qemu.py:55`](../tests/ir_tests/test_qemu.py#L55)), write the fuzz
      memory header; fix the bug; `make test -j16` green.
- [ ] **M7 — Iterate to empty.** Repeat M6 (221, 222, …) until band 0–999 is clean
      on both oracles; then advance the certified frontier (0–4999, …) per
      [master §4 / §7](plan_fuzz_coverage_master.md#4-d2--seed-range-cheap-do-alongside-d1).
- [ ] **M8 — Certify & document.** Record the certified band and per-seed→cause
      rows in the canonical tracker; mark `struct_byval` landed in
      [master §0](plan_fuzz_coverage_master.md#0-where-we-are-baseline).
