# TCC ARMv8-M Codegen — Optimization Plan to Close the GCC Gap

**Target benchmark:** `tests/gcctestsuite/.../gcc.c-torture/execute/pr92904.c`
(variadic functions + struct-by-value passing/returning + 64-bit & double compares).
**Metric:** *static* instruction count, via `scripts/compare_disasm.py` (output in `test.txt`).

| | TCC | GCC | Ratio |
|---|---|---|---|
| Session start | 11245 | 4820 | **2.33x** |
| Current | 10378 | 4820 | **2.15x** |
| `main` (dominant fn) | 9860 | 4548 | 2.17x |

`main` is an unrolled test harness calling `f2`–`f14`; it dominates the count, so
all figures below refer to `main` unless noted. Improvements here generalize to
all struct-passing / 64-bit / multi-guard code.

---

## 1. Measurement & validation methodology

```bash
# Measure (regenerates test.txt; --no-cache avoids touching the cache)
python scripts/compare_disasm.py --no-cache <path>/pr92904.c

# Correctness — pr92904 is AUTO-SKIPPED by the execute pytest harness
# (__builtin_abort / noipa), so run it manually in QEMU:
cd tests/ir_tests && python run.py -c /tmp/pr92904.c --cflags=-O2   # must print "Exit code: 0"

# Full regression gate
make test -j16        # 12329 IR + torture tests, must stay green
make test-asm -j16    # 161 assembler tests
```

### ⚠️ Validation lesson (learned the hard way)
`pr92904` exit-0 is **necessary but not sufficient**. It only catches *spurious*
aborts and *added* faults; it does **not** catch:
- a *wrongly-skipped* abort (control-flow merges), or
- a *miscompile in a pattern it doesn't contain* (e.g. nested struct-returning calls).

**Every control-flow or allocation change MUST ship with targeted hazard tests**
that exercise both directions / the specific aliasing pattern, plus the full
suite. Two attempts this session passed `pr92904` but were wrong (see §4).

---

## 2. Completed levers (2.33x → 2.15x) — for reference

| # | Lever | File | `main` Δ | Notes |
|---|---|---|---|---|
| 1 | Stack-portion struct marshaling → `LDRD`/`STRD` | `arm-thumb-gen.c` (`place_stack_arg_struct`) | −615 | `STRD` to outgoing area always safe; `LDRD` gated on struct natural align ≥4 |
| 2 | Reg-portion of split struct → `LDRD` | `arm-thumb-gen.c` (`THUMB_ARG_MOVE_STRUCT`) | −13 | `ldr r2,[r5];ldr r3,[r5,#4]` → `ldrd r2,r3,[r5]` |
| 3 | 64-bit compare → direct branch | `ir/opt_gens_branch.c` (`ir_gen_setif_branch_fuse`) | −120 | accept `CMP T,#0` form, not just `TEST_ZERO`; general win |
| 4 | `\|\|`-guarded abort tail-merge | `ir/opt_promote.c` (`tcc_ir_opt_abort_tail_merge`) | −119 | 120 `bl abort` → 1; retarget A-branches to shared sink |

Load-bearing constraint discovered: **`load_from_base`'s 64-bit `LDRD` path must
stay SP/FP-base-only** — packed `long long` field access reaches it with a
possibly-unaligned general base; relaxing it faults on hardware.

---

## 3. Where the remaining ~5500-instruction gap lives (in `main`)

Histogram delta vs GCC, ranked by impact:

| Pattern | TCC | GCC | Root cause | Lever |
|---|---|---|---|---|
| `__aeabi_dadd` calls | 240 | 0 | `u.e.a++` (double) computed at runtime; GCC const-folds the deterministic sequence | **C** |
| `add.w sp,#big` (e.g. `&u`=438×, `&v`=120×) | 886 | 136 | frame too big (2804B) → offsets >1020 → can't keep base in reg / can't `ldrd` | **A + B** |
| single `ldr.w [sp,#big]` (64-bit halves) | 1766 | 66 | same — offset >1020 blocks `ldrd` | **A + B** |
| `__aeabi_memmove8` (struct-return copies) | 160 | 0 | not inlined; but inlining barely helps *static* count — real win is base-reg reuse | (B) |

**Frame size is the keystone:** TCC `main` frame = **2804 bytes** vs GCC **316**.
The big frame is what forces offsets > 1020, which simultaneously blocks `ldrd`
and forces the `add.w` address recomputation. Shrinking the frame is a
prerequisite for the addressing wins.

---

## 4. Reverted attempts & lessons (do NOT repeat naively)

### 4a. Within-block frame-address CSE — **regressed +92**, reverted
Replaced a redundant `LEA t<-StackLoc[off]` with `ASSIGN t<-earlier_vreg`.
- **Why it failed:** an `add rN,sp,#off` is a *1-instruction rematerializable*
  address. Turning it into a `mov` from a CSE'd vreg is also 1 instruction (no
  count win) **and** extends the canonical vreg's live range → register
  pressure → extra moves/spills. Coalescing did not collapse the copies.
- **Lesson:** never CSE cheap rematerializable frame addresses into copies. The
  win only exists if you *eliminate* the `add` by addressing dependent
  loads/stores off a persistent base register (→ Lever B).

### 4b. sret-buffer reuse via `get_temp_local_var` — **miscompiled**, reverted
Routed the per-call sret buffer alloc ([tccgen.c:14940](tccgen.c)) through the
reusing temp-local pool.
- **Mechanically worked:** frame.c 92→28B, `main` frame 2804→1620B.
- **But INCORRECT:** hazard test `add(mk(g), mk(g+1))` (nested struct-returning
  calls) aborted at all `-O` — the two sret buffers collided into one slot.
  This is the documented hazard at [tccgen.c:13839](tccgen.c): once the buffer's
  address is taken (passed to the callee), the `VR_TEMP_LOCAL` vstack marker is
  lost, so the pool's liveness heuristic reuses a still-live slot.
- **`pr92904` did NOT catch it** (no nested struct calls). Only the hazard test did.
- **Lesson:** sret-buffer reuse cannot be done at the frontend on vstack
  liveness. It must use real IR liveness (→ Lever A).

---

## 5. Remaining levers (prioritized, with full design)

### Lever A — Liveness-based struct-slot coalescing (shrink the frame) — *recommended first*

**Goal:** reuse one stack slot for non-overlapping struct temporaries (sret
buffers, by-value arg copies), matching GCC (frame 2804→~316).

**Evidence:** 5 sequential `sink=f()` calls use 5 distinct 16-byte slots
(`sp+8,+24,+40,+56,+72`) vs GCC's 1. Each `v.X = fN(...)` in `main` leaves a dead
buffer that the next statement should reuse.

**⛔ FIRST OBSTACLE (found 2026-06): struct-buffer SIZES are discarded after the
frontend.** Coalescing two slots requires knowing each slot's byte size (the
physical slot must be ≥ the largest occupant); coalescing with a wrong size =
memory corruption. But post-frontend nothing carries the true struct size:
`tcc_ls_reg_type_stack_size()` returns 4/8 (scalar), and `tcc_ir_stack_build`
([ir/stack.c:262](ir/stack.c)) also derives size from `reg_type`. **So the
mandatory first implementation step is to plumb buffer sizes** — record
`{offset, size}` at every frontend `loc -= size` temp-buffer site (sret at
[tccgen.c:14940](tccgen.c), by-value struct-arg copies, `?:`/vector temps) into
a side-table on `TCCIRState`. Only then can a coalescing pass run safely.

**Approach (safe — real liveness, not vstack):**
- **Step 1 (blocker):** plumb buffer sizes (above).
- **Step 2:** compute each buffer's *memory* liveness. Either reuse the regalloc
  addrtaken taint analysis ([ir/regalloc.c:1005](ir/regalloc.c) — already tracks
  pointer reach via LEA/ASSIGN/ADD/SUB and conservatively extends escaped
  pointers to function-end), or replicate a self-contained version keyed by
  `StackLoc[off]`.
- **Step 3:** greedily coalesce non-overlapping buffers (size-compatible) into a
  minimal physical slot set; remap `StackLoc[off]` / `Addr[StackLoc[off]]`
  operands and the frame size. The linear scan's 4-byte addrtaken free-list
  ([ir/regalloc.c:1977](ir/regalloc.c)) is the existing precedent (correct via
  the same liveness extension) — but it does NOT cover these fixed-offset
  frontend buffers, so this is a separate pass, not an extension of it.

**Risk:** medium. Correctness rests on the liveness being a correct
over-approximation. **Hazard regression guard LANDED:**
`tests/ir_tests/bug_struct_slot_reuse.c` (nested sret calls, struct-result→arg,
`?:` struct, interleaved sizes — the exact patterns the reverted §4b attempt
miscompiled; `pr92904` does NOT cover them).

**Impact — REVISED:** frame 2804→~316. On its own this only reduces the
instruction metric **if it pushes the frame below ~1020**, which lets the
EXISTING SP/FP-relative `ldrd` path ([arm-thumb-gen.c:4540](arm-thumb-gen.c))
fire on the 64-bit compare loads (currently 1766 `ldr.w` at offsets >1020).

**❌ ATTEMPTED & REVERTED (2026-06).** A full post-IR coalescing pass was built
(`tcc_ir_opt_coalesce_frame_temps`: frontend size side-table + taint-based
memory liveness + same-size coloring + StackLoc remap). It was *mechanically
correct* on simple cases (frame.c 5 sret buffers 92→28 B), **but:**
1. **It MISCOMPILED** — the hazard test caught `acc=388≠382` at -O1/-O2:
   nested struct-returning calls (`addS(mk(a),mk(b))`) keep both sret buffers
   simultaneously live, but the liveness missed the struct-by-value *arg-copy*
   use of the first buffer → wrongly coalesced. (Correctness hole: tracking the
   outgoing-area / BLOCK_COPY copy of a struct passed by value.)
2. **It did NOT move pr92904's metric** — `main` stayed 9860→9859; its frame
   never dropped below 1020 (sret buffers disqualified or other temps dominate).

So Lever A is **both correctness-hard and ~0 metric payoff for pr92904** — not
worth pursuing for this benchmark. (The hazard-test-first methodology worked: it
caught a bug pr92904 misses. `bug_struct_slot_reuse.c` is kept as a guard.)

**Validation:** full suite + hazard suite + a corpus frame-size check (no
function's frame should grow).

---

### Lever B — Base-pointer register promotion (the addressing rewrite)

**Goal:** keep a few hot frame bases (`&u`, `&v`, …) in callee-saved registers
and address fields off them with small displacements — eliminating the 886
`add.w sp,#big` and converting big-offset `ldr.w` pairs to `ldrd`.

**Evidence:** GCC pins `&v`,`&u`,… to `r4/r5/sl/r8` once; TCC recomputes
`add.w rX,sp,#2768` (438×) and `#2736` (120×) and uses `ldr.w [sp,#big]` pairs.

**Approach (entry-hoist, modeled on `tcc_ir_opt_globalsym_cse`
[ir/opt_copyprop.c:797](ir/opt_copyprop.c)):**
- Cluster frame accesses by a base offset B such that many accesses fall in
  `[B, B+W]` (W ≤ 1020 to keep `ldrd`-eligible). Score by use-count.
- For the top-K bases (budget via `tcc_ir_estimate_hoist_budget`), hoist
  `LEA base_k <- StackLoc[B]` to function entry (position 0 → dominates all
  uses, like globalsym_cse's index-shift + jump-target patch).
- Rewrite operands `StackLoc[B+δ]` to base-relative:
  - `LEA Addr[StackLoc[B+δ]]` → `ADD base_k, #δ` (or `ASSIGN` when δ=0)
  - `LOAD/STORE StackLoc[B+δ]` (deref) → `LOAD_INDEXED/STORE_INDEXED [base_k,#δ]`
  - **`CMP a, StackLoc[B+δ]` has no indexed form** — must first materialize the
    load (`LOAD_INDEXED tmp,[base_k,#δ]; CMP a, tmp`) or extend the CMP lowering.
    This is the hardest sub-part.

**Dependency — CORRECTED (2026-06): Lever B does NOT require Lever A.** A base
register holds the big *absolute* address (`r5 = sp+2768`); field displacements
δ stay small (0..63) regardless of frame size, so `[base,#δ]` addressing works
either way. B's win (eliminating the 886 `add.w` via base reuse) is independent.

**⚠️ But B does NOT by itself enable `ldrd`** — verified 2026-06: a 64-bit load
through a *general* base register + small offset still emits two `ldr.w`, not
`ldrd`, because `load_from_base`'s `ldrd` path is **SP/FP-base-only** (the
packed-`long long` alignment hazard — relaxing it faults; see §2). So:
- **B eliminates the `add.w` recomputation** (LEA/base reuse) — its main win.
- **B does NOT pair the loads into `ldrd`** — that needs *either* Lever A
  (frame <1020 → SP-relative `ldrd`) *or* a separate **alignment-info threading**
  change to safely allow general-base `ldrd` when the access is provably aligned
  (hard — packed accesses reach the same path).

**Risk:** high. Pins callee-saved registers function-wide → pressure regressions
elsewhere (globalsym_cse's own comments document regressions, e.g.
`20040709-1::testM`). The `CMP`-operand rewrite touches many op kinds.

**Impact:** eliminates the bulk of the 886 `add.w` (LEA/base reuse). Pairing the
loads into `ldrd` is a *separate* sub-lever (A's frame shrink, or alignment
threading).

**Validation:** full suite + corpus disasm sweep (the regression-measurement
harness referenced in `codegen-gap-vs-gcc` memory) to ensure net-negative, with
an env kill-switch (e.g. `TCC_NO_FRAMEBASE_PROMO`) and a conservative budget.

---

### Lever C — `dadd` const-prop through the union (eliminate the 240 FP ops)

**Goal:** fold `u.e.a++` (and the 64-bit `u.c.a++`) the way GCC does — the values
are a deterministic compile-time sequence (1.25, 2.25, …), so the comparisons
load constants instead of computing `+1.0` at runtime.

**Evidence:** TCC emits 240 `__aeabi_dadd` GCC lacks entirely.

**Approach:** store-to-load forwarding + FP constant folding for an
**address-taken local union**, surviving the by-value calls (which *read* but
don't *write* `u`). Must correctly model the union aliasing (`u.c.a`/`u.e.a`/
`u.g.b` overlap, reset between phases).

**Risk:** very high. Address-taken locals are conservatively "may-alias"; the
union type-punning + FP correctness make this the hardest lever. Likely the
least favorable risk/reward of the three.

**Impact:** ~1400 instructions — the single biggest bucket — but only if done
correctly; an unsound version corrupts floating-point results.

---

## 6. Sequencing & dependencies — CORRECTED (2026-06)

The three levers tackle **disjoint instruction chunks** and are **independent**
(my original "A then B" was wrong — see §5 corrections):

```
   A. Struct-slot coalescing ──► frame <1020 ──► SP-relative LDRD on the
                                                 64-bit COMPARE loads (1766 ldr.w)
   B. Base-pointer promotion ──► eliminates the 886 add.w (LEA/base reuse)
   C. dadd const-prop        ──► eliminates the 240 __aeabi_dadd
   (+ shared sub-lever: alignment-info threading ──► general-base LDRD,
      would also let B pair its loads — but is hard, packed-access hazard)
```

- **No hard ordering.** Land whichever is most tractable/valuable first. A's
  payoff is gated on reaching frame <1020 (needs *full* coalescing, not just
  sret). B's payoff (the `add.w`) is immediate and independent.
- **Recommended order by risk/reward — UPDATED 2026-06:** Lever A was tried and
  is a dead end for pr92904 (miscompiled *and* ~0 metric payoff — see §5). **B is
  now the only directly-metric-moving lever** (the 886 `add.w` are real and
  independent of frame size). Despite its register-pinning risk, B is the path
  forward for this benchmark; C remains the highest-impact-but-riskiest.

---

## 7. Required validation infrastructure (build before landing A/B/C)

1. **Hazard test suite** — **STARTED 2026-06:**
   `tests/ir_tests/bug_struct_slot_reuse.c` (registered, passes -O0/-O1/-O2)
   covers nested struct-returning calls, struct-result→arg, `?:` struct, and
   interleaved different-size buffers — the patterns the reverted §4b attempt
   miscompiled. *Still to add:* struct-array returns, complex returns,
   packed/unaligned struct access, both-direction abort/guard cases.
2. **Corpus disasm regression sweep:** compile the gcc-torture-execute corpus,
   compare per-function instruction counts to a cached baseline, flag any
   regression (the methodology used for the `memclr` levers — see
   `codegen-gap-vs-gcc` memory). A/B must be net-negative across the corpus, not
   just pr92904.
3. **Env kill-switches** for each new pass (A/B/C) for bisection and A/B testing.

---

## 8. Risk register

| Lever | Effort | Correctness risk | Pressure risk | Gating |
|---|---|---|---|---|
| A — slot coalescing | Medium | Medium (liveness windows) | None (stack, not regs) | hazard suite + frame-no-grow |
| B — base-pointer promo | High | Medium (CMP rewrite) | **High** (callee-saved pinning) | budget + corpus sweep + kill-switch |
| C — dadd const-prop | Very high | **High** (FP + union alias) | Low | extensive FP/union tests |

## 9. Honest expectation

This is **not** closeable by surgical edits — it reflects GCC's decades of
stack-allocation, register-allocation, and constant-propagation maturity. A
realistic path to ~1.3–1.5x is **A + B** (frame + addressing), each landed as a
budgeted, corpus-validated pass over multiple focused sessions. Reaching ~1.0x
additionally needs **C**. Two naive attempts this session (§4) prove that
shortcuts here either regress or miscompile; the validation infrastructure in §7
is the gating prerequisite for attempting A/B/C safely.
