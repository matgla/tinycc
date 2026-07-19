# Memory Abstraction Port: route source/ allocations through existing primitives

**Status:** in-progress (Phase 0 + Phase E done; Phase A mostly done — 9 files converted
[const_aggregate, const_var_prop, const_prop_tmp, value_tracking, if_convert, branch, backend
regalloc + arm-thumb-gen, flat vrp (since retired), ssa/cfg/vrp]; 2 reclassified/deferred
[opt_dsl_var_const.h, gvn.c → Phase C]; strategy revised for slow-heap target — `small_sequence`
now default, Phase F added)
· **Branch:** `legacyOptRemoval`

## Goal

Route scope-local dynamic allocations in `source/` through the existing
scope-exit primitives (`defer()`, `scoped_vector(T)`, `small_sequence(T)`) so
that alloc/free is harder to get wrong on future edits (early returns, added
branches), reads closer to RAII, **and — on the target — reduces heap traffic.**

Two distinct wins, kept separate because they map to different primitives:

1. **Safety + readability** (all primitives). `tcc_malloc`/`tcc_realloc` abort on
   OOM (`exit(1)`, `libtcc.c:140`) and never return NULL, and current sites
   already free correctly (leak-sanitizer, `configure:593`, would fail the build
   otherwise). The win is against *future* early-returns and dense alloc/free
   clusters, not against present leaks.
2. **Heap-traffic reduction** (`small_sequence` only). This compiler runs
   **on-target**, where the heap is slow and the process stack is ~32 KB
   (`opt_ssa_domwalk.h:21`). Every compile-time malloc/free in an opt pass is a
   runtime cost. **`scoped_vector` does not help here** — it has no inline
   storage, so the first `resize`/`reserve`/`push_back` calls `tcc_realloc(NULL,
   …)`; converting a fixed-size buffer to it keeps the identical malloc+free.
   Only `small_sequence` (inline-first, heap fallback) *eliminates* the
   allocation when the size fits inline — which, for per-function scratch keyed
   on block/var/tmp counts, is the overwhelmingly common case. On this target
   `small_sequence` is therefore the **default** for scope-local buffers, not an
   optional afterthought (see Pattern 1 / Pattern 4).

**Constraint (both wins):** the ~32 KB target stack means inline-first trades
heap for stack. Size inline caps to cover the *common* case in a few hundred
bytes and let the tail spill to heap; do **not** use maximal caps (a blanket
`int[256]` = 1 KB/frame is unsafe when several coexist or one sits inside the
dom-walk). "Slow heap + tiny stack" is the real constraint — the sweet spot is
*modest* inline caps.

**Non-goal:** eliminating every direct `tcc_malloc`/`tcc_free` in `source/`. That
is unreachable — the abstractions themselves call the real allocator, and
several backend files (`function.c`, `arm_aapcs.c`) are out of scope (they hold
calling-convention scratch, not scope-local pass state). Target is the surviving
`source/opt/` passes plus the two backend hotspots below.

**Scope note (branch `legacyOptRemoval`):** the parallel `ir/*.c` tree is being
**retired** on this branch (net −32,939 lines vs `mob`; passes migrate into
`source/opt/flat/` and `source/opt/ssa/`). This plan targets the **surviving
`source/` tree only.** Earlier revisions of the tracking table listed `ir/*.c`
files — those are struck below; porting them is wasted effort that conflicts
with the removals.

## Existing infrastructure (reuse, do not reinvent)

| Primitive | Header | Covers |
|-----------|--------|--------|
| `defer(fn)` | `source/utils/include/utils/defer.h` | Any scope-exit action (LIFO); free a fixed named pointer at scope end |
| `scoped_vector(T)` | `source/memory/include/memory/vector.h` | Growable arrays (malloc + realloc + free) — full vector API |
| `small_sequence(T)` | `source/memory/include/memory/small_sequence.h` | Inline-first **fixed-size** sequences with heap fallback (init-once) |
| `dynamic_bitset(T)` | `source/memory/include/memory/dynamic_bitset.h` | Bitset operations |
| `unique_ptr(T)` | `source/memory/include/memory/unique_ptr.h` | Single owning pointer (uses `tcc_free()`, fixed in Phase 0) |
| `small_seq_vector(T)` *(planned, Phase F)* | `.../memory/small_seq_vector.h` | Inline-first **growable** sequence (`push_back` spills inline→heap) |

Notes / constraints:

- `defer`'s callback is `void (*)(void)`. It **cannot capture** the pointer
  without a GCC nested function, and nested-function callbacks force an
  executable stack in this codebase. So `defer` maps cleanly onto
  "free this fixed named pointer", not onto capturing cleanups.
- `scoped_vector`/vector methods are `static inline`; unused ones are
  DCE'd (zero size/runtime cost). There is **no need** for a second, "simpler"
  growable-array type.
- The vector API's `-1`-on-failure returns are dead code under the default
  allocator (it aborts on OOM); call sites need not check them.
- **`scoped_vector` always heaps** (no inline storage: first grow calls
  `tcc_realloc(NULL, …)`). It is a *safety*-only tool. `small_sequence` is the
  only *heap-elimination* tool — inline storage, heap only past `inline_cap`.
  On the target, prefer `small_sequence` wherever a bounded common size exists.
- **Missing capability:** there is no *growable* inline-first type.
  `small_sequence` is init-once (`_init` fixes size; no `push_back`). Pattern 2
  growable sites (esp. the shared `opt_ssa_domwalk` worklist) need one — added as
  Phase F (`small_seq_vector`).

## Framework extension: `bitspan` + `bit_matrix` (for dataflow bitsets)

`dynamic_bitset` covers 1-D per-bit use but has no bulk word ops, so the
liveness/dataflow bitsets in `ir/regalloc.c` (a flat `nb`×`nw` matrix with
SWAR kernels and the `RA_BS_*` macros) fit neither it nor `scoped_vector`. Add a
two-level, header-only extension. Both layers are `static inline` / macros — no
`.c`, no Makefile or UT-link changes; the hot loop stays word-level with no
bounds checks. **Headers + unit tests landed** (see Phase E status).

### Layer 1: `bitspan` — word-op layer (`source/memory/include/memory/bitspan.h`)

General over any `uint64_t[]` run; also covers the 1-D `isint`/`live` bitsets.
Per-bit `set/test/reset` here replace the `RA_BS_*` macros directly.

| Function / macro | Semantics | Replaces |
|------------------|-----------|----------|
| `tcc_bitspan_zero(w, n)` | `w[..] = 0` | `for w: lo[w]=0` |
| `tcc_bitspan_copy(d, s, n)` | `d = s` | `live[w]=lo[w]` |
| `tcc_bitspan_or(d, s, n)` | `d |= s` | `lo[w] |= sli[w]` |
| `tcc_bitspan_or_andnot(d, gen, in, kill, n) → int` | `d = gen\|(in&~kill)`; returns 1 if any word changed | liveness transfer `nv = ub\|(lo&~db)` + changed flag |
| `tcc_bitspan_and_popcount(a, b, n) → int` | `popcount(a & b)` | `p += popcount(live[w]&isint[w])` |
| `tcc_bitspan_set/reset/test(w, bit)` | per-bit ops | `RA_BS_SET/CLR/TEST` |
| `tcc_bitspan_for_each_set(w, n, limit, idx) { … }` | iterate set-bit indices `< limit` via ctz | `FOR_LIVE_CAND` |

### Layer 2: `bit_matrix` — single-allocation 2-D owner (`.../bit_matrix.h`)

Template pattern matching `dynamic_bitset`/`vector` (instantiate a named type;
allocator is swappable for tests). Not a pre-made concrete type.

```c
TCC_BIT_MATRIX_DEFINE(RaLiveMatrix)  // uses tcc_mallocz/tcc_free
// or TCC_BIT_MATRIX_DEFINE_WITH_ALLOCATOR(name, allocate, deallocate)

int  RaLiveMatrix_init(RaLiveMatrix *m, int rows, int columns); // one alloc; -1 on overflow/negative
void RaLiveMatrix_cleanup(RaLiveMatrix *m);
uint64_t *RaLiveMatrix_row(RaLiveMatrix *m, int row);           // words + (size_t)row*words_per_row
void RaLiveMatrix_set/reset(RaLiveMatrix *m, int row, int bit);
int  RaLiveMatrix_test(const RaLiveMatrix *m, int row, int bit);

#define bit_matrix(name) name __attribute__((cleanup(name##_cleanup)))
```

- `_init` clears storage itself (allocator need not zero); columns round up to
  `words_per_row`; returns `-1` on negative dims or size overflow.
- Caller keeps its own domain soft-cap (`(long)nb*nw > 4<<20`, `:873`) before init.
- `bit_matrix(name)` auto-frees on every path — kills the early-return leak
  spots at `:982`/`:1092`.

Post-migration kernel shape:
```c
TCC_BIT_MATRIX_DEFINE(RaLiveMatrix)
...
bit_matrix(RaLiveMatrix) useb = {0}, defbk = {0}, livein = {0}, liveout = {0};
if (RaLiveMatrix_init(&liveout, nb, tbl) != 0) { ...bail... }
uint64_t *lo = RaLiveMatrix_row(&liveout, b);
tcc_bitspan_zero(lo, nw);
for (succ sb) tcc_bitspan_or(lo, RaLiveMatrix_row(&livein, sb), nw);
changed |= tcc_bitspan_or_andnot(li, ub, lo, db, nw);
```

Consumers today: `ir/regalloc.c` only (2 functions × 4 matrices + `isint`/`live`).
Accepted as a one-file consumer now; the `bitspan` layer is general and the SSA
dataflow passes are the expected future users.

## Allocation patterns in source/

**Census (corrected).** The whole repo has ~517 alloc + ~566 free calls across
~40 files spanning both the `ir/` (retiring) and `source/` (surviving) trees. The
in-scope surface — the **compiled** `source/` passes + backend hotspots — is
~170 alloc/free calls. Two earlier counts in this doc were wrong and are
corrected here:

- The "274 across 26 files" figure conflated trees and is dropped.
- `source/opt/regalloc_pipeline.c` (66 sites) is a **git-tracked orphan** — not
  in any Makefile, not `#include`d, no `.o` (same finding as Phase E). All its
  sites below are struck; the live regalloc code is `ir/regalloc.c`.

Authoritative compiled `source/opt` passes: `FLAT_OPT_SRC`
(`source/opt/flat/Makefile`) + `SSA_OPT_SRC` (`source/opt/ssa/Makefile`) +
`source/opt/function_pipeline.c`. Anything under `source/opt` not listed there is
not built — verify against those Makefiles before porting a site.

Grouped by migration strategy (not by call shape):

### Pattern 1: scope-local fixed-size array — `small_sequence(T)` (default on target)

One (or a few) `tcc_mallocz(sizeof(T) * n)` sized once from a per-function bound
and used within a single function, freed at scope exit. This is the largest
group, and on the slow-heap target it is also **the biggest heap-traffic win** —
`n` is small for the overwhelming majority of functions, so an inline-first
buffer pays no heap at all in the common case.

```c
uint8_t *live_vregs = tcc_mallocz((max_vreg_pos + 8) / 8);
// ... use ...
tcc_free(live_vregs);
```

**Default migration: `small_sequence(T)`, not `scoped_vector`.** `scoped_vector`
has no inline storage — it would keep the exact malloc+free (safety only, zero
heap win; see Goal). `small_sequence` sized once via `_init` gives the same
auto-cleanup *and* eliminates the allocation when `n ≤ inline_cap`:
```c
TCC_SMALL_SEQUENCE_DEFINE(LiveVregs, uint8_t, 64)  // 64 B inline → 512 vregs, spills past that
...
small_sequence(LiveVregs) live = {0};
if (LiveVregs_init(&live, (max_vreg_pos + 8) / 8) != 0) { ...bail... }
uint8_t *p = LiveVregs_data(&live);   // inline or heap, transparently
// auto-freed at scope exit
```
Choose `inline_cap` per the ~32 KB stack budget (Goal): cover the common case in
a few hundred bytes, not maximal. `scoped_vector` remains the right tool only for
genuinely large/unbounded fixed buffers where inline storage would blow the stack
(rare here — most are `(max+8)/8` bitsets or `sizeof(T)*(max+1)` arrays with
small typical `max`).

Compiled candidate sites (surviving tree):
- `source/opt/flat/scalar/const_var_prop.c` — **6** `(max_var+8)/8` bitsets
  (`:60`, `:61`, `:100`, `:116`, `:151`, `:461`)
- `source/opt/flat/scalar/const_prop_tmp.c:547-548` — `var_info`, `var_addrtaken`
  (siblings of the already-hand-rolled inline buffer at `:521`)
- `source/opt/flat/scalar/value_tracking.c:113` — `pred_count` (`int[n]`)
- `source/opt/flat/cfg/if_convert.c:238` — `jt_cnt` (`int[n]`)
- `source/opt/framework/opt_dsl_var_const.h:66` — `def_idx` (`int32_t[nv]`)
- `source/opt/ssa/scalar/gvn.c:914` — `entry_pool`; `source/opt/ssa/cfg/branch.c:716` — `worklist`
- `source/backend/generators/regalloc.c:440`, `source/backend/arch/arm/thumb/arm-thumb-gen.c:2556`

`arm-thumb-callsite.c` already proves the pattern (6 live `small_sequence`s);
reuse the `THUMB_CALL_INLINE_ARGS` cap convention. Multi-alloc-per-line clusters
that read worse as scoped vars — leave those as manual malloc/free.

### Pattern 2: scope-local growable array — `small_seq_vector` (new) / `scoped_vector(T)`

```c
OptSSADomWalkItem *stack = tcc_malloc(sizeof *stack * cap);
// ... grow via tcc_realloc ...
tcc_free(stack);
```

Sites (compiled tree):
- `source/opt/framework/opt_ssa_domwalk.h:57-94` — **highest leverage**
- `source/opt/flat/scalar/const_var_prop.c:279`
- `source/opt/ssa/memory/global_addr_hoist.c:117`
- `source/opt/ssa/memory/load_cse.c:717`, `:1555-1557` (work/snap growth)
- `source/opt/ssa/scalar/gvn.c:171`
- `source/opt/ssa/cfg/cmp_eq.c:39`

**Capability gap.** `small_sequence` is **init-once** — it fixes size at `_init`
and has no `push_back`/grow that migrates inline→heap, so it cannot cover these.
`scoped_vector` covers them but always heaps (safety only, no slow-heap win).
The highest-leverage site, `opt_ssa_domwalk.h`, runs **once per pass per
function and is shared by every dominator-tree pass** (gvn, cmp_eq, and future
SSA dataflow); its worklist is ≤16 for almost all functions. A growable
inline-first type would drop that malloc+free for essentially the whole corpus —
probably the single largest heap reduction in the pipeline.

Migration: the small/common sites go to a new growable inline-first type
(**Phase F** — `small_seq_vector`, llvm `SmallVector` semantics); genuinely
large/unbounded ones stay `scoped_vector(T)` + `vector_push_back`. No
`scoped_growable_array` type is created (that is `scoped_vector`).

### Pattern 3a: scope-local multi-array struct — `defer()`

A struct whose sub-arrays are all allocated **and freed inside the same
function**. Candidate for a single free-thunk that releases every sub-array.

### Pattern 3b: cross-function pass-state object — LEAVE AS EXPLICIT new/free

The struct is allocated in one function, **returned**, and freed in a separate
function. `__attribute__((cleanup))` does **not** apply (it needs a scope-local
auto variable; a returned cleanup-scoped stack struct would be freed on return).

Concrete example — `known_bits.c`:
```c
KBState *st = tcc_mallocz(sizeof(KBState)); // kb_alloc()
return st;
// ... used across the pass ...
tcc_free(st->tmp_kb); ... tcc_free(st);     // kb_free()
```
These keep their explicit `new`/`free` pair. Optionally introduce a paired
`kbstate_new()`/`kbstate_free()` helper for symmetry, but **not** a cleanup
attribute. Applies to the "pass state" structs in `known_bits.c`,
`value_tracking.c`, `cprop.c`, `load_cse.c`, `gvn.c`, `global_addr_hoist.c`,
`const_prop_tmp.c`, `var_tmp_fwd.c`, etc. — classify each before touching it.

### Pattern 4: inline-first with heap fallback — `small_sequence(T)`

```c
TmpConstInfo tmp_info_stack[TMP_CONST_STACK_SIZE];
...
if (small) { use stack } else { heap_alloc = tcc_mallocz(...); }
if (heap_alloc) tcc_free(heap_alloc);
```

Sites:
- `source/opt/flat/scalar/const_prop_tmp.c:521-608`
- `source/opt/flat/scalar/value_tracking.c:113-182`

`small_sequence(T)` already encodes exactly this. These two are the *explicit*
inline-first sites; note that on the target the same primitive is now the
**default** for Pattern 1 (fixed-size) too — so Pattern 4 is no longer a separate
"lowest priority" bucket, it is the tail of the Pattern 1 work. `const_prop_tmp.c`
is the exemplar: `:521` already hand-rolls the stack-then-heap split, so
replacing it with `small_sequence` is a pure readability/consistency win, and its
`:547-548` siblings (Pattern 1) fold into the same conversion.

## Implementation plan

### Phase 0: fix `unique_ptr` allocator mismatch ✅ DONE

`source/memory/unique_ptr.c:46,63` called bare `free()`. Switched to `tcc_free()` so
the type is safe for `tcc_malloc`'d pointers. No users today → done first, cheap.

Files changed:
- `source/memory/unique_ptr.c` — `free()` → `tcc_free()`, updated includes
- `tests/unit/arm/armv8m/test_unique_ptr.c` — `__wrap_free` → `__wrap_tcc_free`
- `tests/unit/arm/armv8m/stubs_unique_ptr.c` — new minimal stubs for UT12
- `tests/unit/arm/armv8m/Makefile` — added stubs file, changed `--wrap=free` → `--wrap=tcc_free`

Test results: 34 tests, 364 asserts, 0 failed.

### Phase A: Pattern 1 → `small_sequence` (largest; safety **and** heap win)

Convert scope-local fixed-size buffers to `small_sequence` (default) — inline
storage removes the heap round-trip for the common small case, with auto-cleanup.
Prioritize by (a) call frequency and (b) early returns between alloc and free.
Fall back to `scoped_vector` only where no bounded common size exists / inline
storage would threaten the ~32 KB stack. Skip multi-alloc-per-line clusters.
Pattern 4 (explicit stack-then-heap sites) is the tail of this phase.

### Phase B: Pattern 2 → Phase F type (small/common) / `scoped_vector` (large)

Convert scope-local growable arrays. `opt_ssa_domwalk.h` is the highest-leverage
site (shared by every dom-tree pass) — but it needs the growable inline-first
type from **Phase F**, so sequence B after F (or land F's type first, then do B).

### Phase C: Pattern 3 — classify, then act

Split each site into 3a (scope-local) or 3b (cross-function). Migrate 3a to a
free-thunk/`defer`; leave 3b as explicit new/free (optionally add a paired
helper). Do **not** build a generic `scoped_struct` — it cannot bind the right
per-type cleanup and would double-free heap-vs-stack structs.

### Phase D: folded into Phase A

Pattern 4's two explicit inline-first sites (`const_prop_tmp.c`,
`value_tracking.c`) are now done as part of Phase A — same primitive, same
target. Kept as a label for the tracking table only.

### Phase F: growable inline-first type — `small_seq_vector` (NEW)

Add a header-only growable inline-first sequence (llvm `SmallVector` semantics):
inline buffer of `N`, `push_back`/`reserve` that migrates inline→heap on first
overflow, `__attribute__((cleanup))` free, allocator swappable for tests. This is
the missing capability that unblocks the slow-heap win on Pattern 2. Model it on
the existing `small_sequence` template (same file conventions, UT wiring, and
`TCC_..._DEFINE`/`TCC_..._DEFINE_WITH_ALLOCATOR` split); add unit tests mirroring
`test_bitspan`/`test_bit_matrix` (inline→heap transition, cleanup, move,
overflow/size caps). First consumer: `opt_ssa_domwalk.h` (cap ~16). Independent
of A–E; do it before Phase B.

### Phase E: dataflow bitsets → `bitspan` + `bit_matrix` ✅ DONE

Two-level extension built, then the live liveness kernels migrated.

**Migration target correction:** the plan originally cited
`source/opt/regalloc_pipeline.c`, but that file is a git-tracked **orphan** —
not in any Makefile, not `#include`d, no `.o`. The compiled liveness kernels
live in `ir/regalloc.c` (`ra_refine_live_regs_accurate:4029`,
`ra_coalesce_graph:4146`, `RA_BS_*` macros at `:4007`). The migration targeted
`ir/regalloc.c` (user-confirmed).

1. ✅ Add `source/memory/include/memory/bitspan.h` (header-only).
2. ✅ Add `source/memory/include/memory/bit_matrix.h` (header-only, template).
3. ✅ Unit tests — `tests/unit/arm/armv8m/test_bitspan.c` (8 tests: word-op
   correctness inc. `or_andnot` changed-flag, `and_popcount`, per-bit ops, and
   the ctz-iterate ascending/limit/empty boundaries) and `test_bit_matrix.c`
   (7 tests: single-alloc + self-zeroing, words-per-row rounding, empty dims,
   row independence/contiguity, scoped auto-cleanup, negative-dim rejection).
   Wired into UT12 (`build_unique_ptr`); suite 49/49 green, clean `-Werror`.
4. ✅ `ir/regalloc.c`: instantiated `TCC_BIT_MATRIX_DEFINE(RaLiveMatrix)`; both
   functions' 4 matrices (`useb`/`defbk`/`livein`/`liveout`) → `bit_matrix`
   (single alloc + auto-cleanup, removing all manual matrix frees incl. the two
   early-return paths); all `RA_BS_SET/CLR/TEST` → `tcc_bitspan_set/reset/test`;
   word loops → `tcc_bitspan_zero/copy/or/or_andnot/and_popcount`; the
   `FOR_LIVE_CAND` macro → `tcc_bitspan_for_each_set`. `isint`/`live` kept as
   manual `tcc_malloc` buffers driven by `bitspan` (focused diff). `RA_BS_*`
   macros removed.
5. ✅ `(long)nb*nw > 4<<20` soft cap retained at the `ra_coalesce_graph` call site.

**Validation:** `make test -j32` → 13635 passed / 161 skipped / 1 xfailed
(incl. QEMU execution + gcc-torture execute), frontend/linker/debug/runtime all
green. Behavior-preserving refactor → codegen expected byte-identical.
Fuzz sweep deferred to the user per project policy.

## Validation

1. **Build:** `make cross` after each phase.
2. **Test:** `make test -j16` after each phase (delta check vs baseline).
3. **Sanitizers:** ASan/leak already default via `configure`; a leak fails tests.
4. **Fuzz:** deferred to the user per project policy (no self-run sweeps).

## Open questions

- [x] `unique_ptr` `free()` → `tcc_free()` — yes, Phase 0.
- [ ] Any `opt/` allocation site that fits none of Patterns 1–4? (audit during
      each phase; extend this doc if found.)
- [x] Are the `ir/regalloc.c` bitset arrays (`useb`/`defbk`/`livein`/
      `liveout`) better served by `dynamic_bitset(T)` than raw `uint64_t[]`?
      **Not by `dynamic_bitset` (1-D, per-bit only), but yes by a new 2-level
      extension — see Phase E.** They are a flat 2-D bit-matrix (`nb`×`nw`) driven
      by word-level SWAR (union / and-not / masked popcount / ctz-iterate) in a
      fixpoint loop. The fitting abstraction is a `bitspan` word-op layer plus a
      single-allocation `bit_matrix` owner, not `dynamic_bitset`.

## Files to port (tracking)

`[~]` done · `[ ]` pending · `[x]` skipped · `[!]` blocked

### Phase 0 — `unique_ptr` allocator fix (DONE)

- `[~]` `source/memory/unique_ptr.c` — `free()` → `tcc_free()`
- `[~]` `tests/unit/arm/armv8m/test_unique_ptr.c` — `__wrap_free` → `__wrap_tcc_free`
- `[~]` `tests/unit/arm/armv8m/stubs_unique_ptr.c` — new minimal stubs for UT12
- `[~]` `tests/unit/arm/armv8m/Makefile` — added stubs, `--wrap=tcc_free`

**Scope reset:** only **compiled `source/` files** are listed below. The `ir/*.c`
entries from earlier revisions are removed — that tree is being deleted on this
branch (see Scope note). Cross-check every path against `FLAT_OPT_SRC` /
`SSA_OPT_SRC` before porting.

### Phase A — Pattern 1 + Pattern 4: scope-local fixed-size → `small_sequence`

- `[~]` `source/opt/flat/scalar/const_aggregate.c` — 8 fixed-size buffers (`ta`, `is_jt`,
  `has_back_pred`, `need_save`, `cur`, `tcv`, `tck`, block-scoped `acc`) → `small_sequence`
  (`CAggTmpArr`/`CAggSlotArr`/`CAggI64Arr`/`CAggByteArr`); growable `escaped` → `scoped_vector`
  (zero-alloc in the common no-escape case, a heap win vs the old unconditional 64 B malloc).
  Sparse per-instr `saved` snapshot store left manual (Pattern 3). Header prose pruned to a
  one-paragraph banner. Object-diff **0 changed functions** at -O2 (all suites); `make ut`
  (`opt_const_aggregate` 15/15, leak-checked) + `make test` (13,635) green.
- `[~]` `source/opt/flat/scalar/const_var_prop.c` — 5 `(max_var/tmp+8)/8` bitsets
  (`var_read`, `tmp_read`, `var_addr_taken`, `has_live_lea`, `seen`) + `has_use` → one
  `CvpBitset` (uint8_t, cap 64). `tmp_read` keeps NULL-ness (init only when max_tmp≥0). Growable
  `var_info` left manual (Phase B). *(Also, separately: the `var_read` and `var_addr_taken`
  read-scans were fused into one forward walk — compile-time cleanup, not allocation; tracked in
  `docs/plan_legacy_flat_ir_ssa_retire.md` loop-analysis note.)*
- `[~]` `source/opt/flat/scalar/const_prop_tmp.c` — replaced the hand-rolled stack-then-heap split
  (`tmp_info`/`block_start_seen`, old `TMP_CONST_STACK_SIZE/N` macros removed) + siblings
  `var_info`/`var_addrtaken` → `TmpInfoSeq`/`CptIntSeq`/`CptByteSeq` (caps 64/256/64 match the old
  inline sizes). Now **zero** raw allocs.
- `[~]` `source/opt/flat/scalar/value_tracking.c` — local `pred_count` (`int[n]`) → `VtIntSeq`
  (cap 256). The `a->`/`c->` struct members are cross-function pass-state (Pattern 3b, → Phase C).
- `[~]` `source/opt/flat/cfg/if_convert.c` — `jt_cnt` (`int[n]`) → `IfcIntSeq` (cap 256); auto-cleanup
  also closes any latent early-return leak.
- `[!]` `source/opt/framework/opt_dsl_var_const.h` — **deferred / reclassified.** The 4 arrays
  (`def_idx`/`def_cnt`/`blocked`/`use_cnt`) are built in `_state_build` and freed in `_state_free`
  (a pass-state object, Pattern 3-shaped). small_sequence owners would have to live **in the struct**;
  raw-pointer-into-inline-storage aliases dangle if the struct is ever copied — footgun. Leave until
  Phase C decides the struct-embed vs raw-pointer question.
- `[!]` `source/opt/ssa/scalar/gvn.c` — **deferred / reclassified.** `entry_pool` (and
  `param_mutated`/`undo_stack`) are **file-scope globals** shared across `gvn_param_scan`/`gvn_visit`,
  not scope-local — `small_sequence` (scope-cleanup) doesn't fit without threading them through a
  struct. Belongs with the gvn pass-state work (Phase C), not Phase A.
- `[~]` `source/opt/ssa/cfg/branch.c` — `worklist` (`int[nb]`) → `BranchIntSeq` + `seen_pred`
  (`uint8_t[nb]`) → `BranchU8Seq` (caps 128). Returned `reachable` left manual (owned by caller).
- `[~]` `source/backend/generators/regalloc.c` — `live_vregs` (`(max_vreg+8)/8` bitset) →
  `RaLiveVregBitset` (cap 64).
- `[~]` `source/backend/arch/arm/thumb/arm-thumb-gen.c` — `literal_positions` (`int[pool_count]`) →
  `ThumbLitPosSeq` (cap 128). Other allocs there are growable/pass-state (out of Phase A scope).

**Phase A batch verification (2026-07-15):** all conversions above landed together — combined
object-diff **0 changed functions** at -O2 (all suites, vs pre-batch baseline), `make ut`
(2704 tests, leak-sanitized) + `make test` (13,635 passed) green.

- `[~]` `source/opt/flat/scalar/vrp.c` — **follow-on (2026-07-15), after the vrp relocation** (see
  `docs/plan_legacy_flat_ir_ssa_retire.md`). The two fixed 3\*256-slot `VRPRange` range tables (`ranges`,
  `deferred_ranges`, ~18 KB each) → `VrpRangeSeq` (`small_sequence`, **inline cap 1** so they always spill
  to the heap — matching the original `tcc_mallocz`; the point here is slow-heap RAII/leak-safety, not
  inline storage). Owners declared **before** the `is_merge` helper alloc so the defensive (unreachable)
  init-fail `return 0` can't leak it; both `tcc_free`s dropped (auto-cleanup). `is_merge` stays manual —
  returned by `ir_opt_build_merge_bitmap`, not a scope-local alloc. Object-diff **0 changed functions** at
  -O2 (all suites); `make ut` (`opt_vrp` 24/24, leak-sanitized, 12 binaries) + `make test` (13,635) green.
  *(File since retired with the flat vrp pass — row kept for the cap rationale only.)*
- `[~]` `source/opt/ssa/cfg/vrp.c` — **follow-on (2026-07-17).** All 4 raw allocs removed (zero left).
  Fixed-size → `small_sequence`: `ranges` → `SVRangeSeq` (`SVRange`, **cap 64** = 768 B after the
  struct was packed 24→12 B, see the readability refactor below), `param_stable`/`var_stable` →
  `SVFlagSeq` (`uint8_t`, **cap 32** = 32 B each); 832 B inline total in the one frame. **Caps are
  measured, not guessed** — see the measurement note below. The `SVState`
  struct keeps its raw `ranges`/`*_stable`
  pointers as aliases into the owners (`small_sequence` is init-once and never moves after `_init`, and
  the owners outlive the domwalk) — so the hook bodies are unchanged. NULL-ness is not load-bearing here
  (`sv_slot` bounds-checks `*_cap` before touching the flag arrays), so the `tmp_read` gotcha does not
  apply. **Also took the growable `log`** (hand-rolled `tcc_realloc` doubling, a Phase B/Pattern 2 site)
  → `TCC_VECTOR_DEFINE(SVUndoVec)` + `scoped_named_vector`, since `small_sequence` is init-once: the
  owner lives in `ssa_opt_vrp` and `SVState` holds `SVUndoVec *log`, which drops the `log_count`/`log_cap`
  fields (watermark = `log->size`, undo = `_pop_back`). Chose `TCC_VECTOR_DEFINE` (all `static inline`)
  over `scoped_vector(T)` to avoid a UT link dep on `source/memory/vector.c`. Growth curve changes (4×2
  vs 64×2) — heap-traffic only, not codegen. All 4 `tcc_free`s dropped; `ssa_opt_vrp` now tail-returns
  `opt_ssa_domwalk`. `make cross` clean at `-Werror`; user confirmed tests green.

  **Cap measurement (method worth reusing for every Phase A/B cap).** Temporarily `fprintf`'d
  `cap/temp_cap/param_cap/var_cap` from `ssa_opt_vrp`, compiled all 1,682 `gcc.c-torture/execute`
  files at `-O2` → **28,407 calls**. Result — the distribution is *bimodal with a hard knee*, not a
  smooth tail:

  | `ranges` cap | 16 | 32 | 48 | **64** | 128 |
  |---|---|---|---|---|---|
  | calls fully inline | 19.5% | 95.8% | 97.4% | **97.9%** | 98.8% |
  | inline bytes @ 24 B/slot | 384 | 768 | 1152 | 1536 | 3072 |
  | inline bytes @ 12 B/slot (packed) | 192 | 384 | 576 | **768** | 1536 |

  73% of all calls sit at exactly 26 or 28 slots (`cap` mode), so **cap 16 lands just below the mode
  and wins almost nothing** (19.5%); cap 32 clears it (95.8%) and 64 reaches 97.9%. `param_cap`
  **maxes at 13 across the entire corpus** (cap 32 → 100%); `var_cap` cap 32 → 99.8%. Lesson: the
  plan's "cover the common case, not the max" rule needs the *actual mode* — the guessed cap 16
  (from the `sizeof(T)≲512 B` rule alone) was the worst of both worlds, and the guessed flag cap 64
  was 2× oversized for arrays that never exceed 13. Packing `SVRange` 24→12 B then bought cap 64
  (97.9%) for the same 768 B that cap 32 (95.8%) cost unpacked.

  **Readability refactor (same session, 2026-07-17).** Assessed `ssa:vrp` for a DSL port first —
  **it does not fit, and this is structural, not effort**: (1) `IRSSAOptGen` is
  `{int op; ssa_gen_fn fn; const char *name;}` and `ssa_opt_run_gens` dispatches opcode→`fn(ctx, i)`
  with **no state/begin/end hook** (the SSA side lacks even flat's `run_stateful_gens`), so a
  dom-scoped range map has nowhere to live; (2) `PAIR`/`opt_dsl_pair_match` links only through
  **vreg def-use** (`vi->def_instr`), but CMP+JUMPIF is a **flag** dependency with no vreg edge;
  (3) `REWRITE` mutates instruction `i` only, while every vrp fold mutates *two* instructions plus
  CFG state (`ssa_drop_phi_edge`). The DSL is for opcode-triggered peepholes; vrp is dominator-scoped
  dataflow. Done instead, all four codegen-neutral:
  - **`sv_jumpif_edges()`** — the target/fall-through resolution (read `dest.u.imm32`, bounds-check
    against `num_instrs`, skip NOPs, bounds-check again) was copy-pasted **3×**; now one helper reusing
    the existing `ir_skip_nops_forward`. This is exactly the code that caused the `instr_to_block` ASan
    OOB class, so 3 copies → 1 shrinks that surface.
  - **`SVCmp` + `sv_read_cmp()`** — the ~8-line `CMP x,#c` decode preamble was repeated in
    `_jumpif`/`_setif`/`_select`/`sv_enter` (**4×**). `c_ok` is reported rather than rejected because
    the tautology-vs-0 fold needs no constant.
  - **`sv_seed_def` split** (107 lines → a 20-line dispatcher + `sv_seed_assign`/`_addsub`/`_bitop`).
  - **`SVRange` packed** `{int valid; int64_t lo,hi}` (24 B) → `{int32_t lo,hi; uint8_t valid}` (12 B);
    `SVUndo` 32→16 B. Safe because every seeding path already clamps to int32 — and now **`sv_set`,
    the sole writer, enforces it**: an out-of-domain fact is dropped, not truncated (unreachable today,
    but the guard is what makes the narrowing future-proof).

  **Verification (the strong gate for a behavior-preserving refactor):** object-diff vs the
  pre-refactor build — **0 changed functions / 20,429 unchanged** at -O2 over all suites (the ±2 NEW
  `gcc-execute/*::main` are the documented harness nondeterminism). `make test` 13,550 passed /
  246 skipped / 1 xfailed. **Note on that count:** it is *not* comparable to the 13,635/161 recorded
  elsewhere in this doc — those runs used a non-ASan `config.mak`; under ASan, 85 QEMU/torture
  *execution* tests self-skip as "too slow" (81 `test_gcc_torture_ir` + 4 `test_qemu`), and
  13,550+246 == 13,635+161 == 13,796 total. Byte-identical codegen makes those execution tests
  redundant anyway.

### Phase B — Pattern 2: scope-local growable → Phase F type / `scoped_vector`

- `[ ]` `source/opt/framework/opt_ssa_domwalk.h` — :57-94 worklist (**highest leverage**; needs Phase F)
- `[ ]` `source/opt/flat/scalar/const_var_prop.c` — :279 grow pattern
- `[ ]` `source/opt/ssa/memory/global_addr_hoist.c` — :117
- `[ ]` `source/opt/ssa/memory/load_cse.c` — :717, :1555-1557 (work/snap growth)
- `[ ]` `source/opt/ssa/scalar/gvn.c` — :171
- `[ ]` `source/opt/ssa/cfg/cmp_eq.c` — :39

### Phase C — Pattern 3: classify 3a (scope-local) vs 3b (cross-function)

- `[ ]` `source/opt/flat/scalar/value_tracking.c` — pass-state struct
- `[ ]` `source/opt/flat/scalar/known_bits.c` — pass-state struct (3b)
- `[ ]` `source/opt/flat/scalar/var_tmp_fwd.c` — pass-state struct (3b)
- `[ ]` `source/opt/ssa/scalar/cprop.c` — pass-state (18 allocs — heaviest; :137,:992-993,:1038-1039)
- `[ ]` `source/opt/ssa/scalar/gvn.c` — pass-state (3b)
- `[ ]` `source/opt/ssa/memory/global_addr_hoist.c` — pass-state
- `[ ]` `source/opt/ssa/memory/load_cse.c` — pass-state (cross-function snaps)

### Phase D — folded into Phase A (label retained)

The Pattern 4 explicit stack-then-heap sites (`const_prop_tmp.c:521`,
`value_tracking.c`) are tracked under Phase A above.

**Inline-capacity recommendations (bounded by the ~32 KB target stack).** Cover
the common case, not the max; keep each buffer to a few hundred bytes:
- Bitsets (`uint8_t[]`): ~64 B inline → 512 blocks/vars (was "~256 elements")
- Int arrays (`int[]`): ~64 elements = 256 B (not 256 elements = 1 KB)
- Struct arrays: size so `inline_cap * sizeof(T)` ≲ 256-512 B; verify `sizeof(T)`
- Multiple inline buffers in one frame: sum them against the budget before choosing caps

### Phase E — dataflow bitsets → `bitspan` + `bit_matrix` ✅ DONE (`ir/regalloc.c`)

- `[~]` `source/memory/include/memory/bitspan.h` (DONE)
- `[~]` `source/memory/include/memory/bit_matrix.h` (DONE)
- `[~]` `tests/unit/arm/armv8m/test_bitspan.c` (DONE, 8 tests)
- `[~]` `tests/unit/arm/armv8m/test_bit_matrix.c` (DONE, 7 tests)
- `[~]` `ir/regalloc.c` — matrices → `bit_matrix`, `RA_BS_*`/word loops → `bitspan` (DONE)

### Phase F — growable inline-first type (NEW)

- `[ ]` `source/memory/include/memory/small_seq_vector.h` — header-only template (`push_back` spills inline→heap)
- `[ ]` `tests/unit/arm/armv8m/test_small_seq_vector.c` — inline→heap transition, cleanup, move, caps; wire into UT12
- `[ ]` `source/opt/framework/opt_ssa_domwalk.h` — first consumer (cap ~16)

## Execution order

1. Phase 0 — `unique_ptr` allocator fix ✅
2. Phase E — dataflow bitsets → `bitspan` + `bit_matrix` ✅ (independent)
3. Phase A — Pattern 1 + Pattern 4 → `small_sequence` (fixed-size; safety + heap win)
4. Phase F — growable inline-first `small_seq_vector` (unblocks B)
5. Phase B — Pattern 2 → Phase F type / `scoped_vector`
6. Phase C — Pattern 3 (classify 3a/3b)
7. Final: `make test` delta clean; hand to user for fuzz sweep
