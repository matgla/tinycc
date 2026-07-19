# -O2 Code-Size and Speed Improvement Plan

**Status**: P1 landed (except the §3.7 jump-table admission heuristic), P2
partially, P3 landed in full (forward narrowing + cbz), P4 step 1, §3.6
return-pair — see §7
**Created**: 2026-07-20
**Branch measured**: `legacyOptRemoval` @ `bc0e02ce`
**Goal**: Close the remaining `-O2` gap to `arm-none-eabi-gcc -O2` on ARMv8-M,
in both `.text` bytes and executed cycles.

Every number in this document was measured on this tree with the commands
given in §1. No estimate is carried over from an earlier plan.

Scope note: this plan is about **the quality of the code we emit**. The
optimizer's own compile-time cost and structure are a separate track —
see [opt_pass_dedup_and_perf.md](opt_pass_dedup_and_perf.md). Pass-migration
status lives in [legacy_opt_porting_status.md](legacy_opt_porting_status.md).

---

## 1. How this was measured (reproduce before trusting)

```bash
make cross -j16

# per-function instruction counts, TCC -O2 vs GCC -O2, 4151 tests
./scripts/regression_disasm.py --suite all -j16          # summary
./scripts/regression_disasm.py --suite all -j16 --csv    # per-function rows

# deterministic cycle counts for the generated code
python3 metrics/qemu_corpus.py --opt=-O2 --suite ir -j12

# both sides of a diff at once (correctness + size + cycles)
python3 metrics/compare_worktree.py
```

Single-idiom probes were compiled both ways and disassembled:

```bash
./armv8m-tcc -O2 -c probe.c -o t.o
arm-none-eabi-gcc -O2 -mcpu=cortex-m33 -mthumb -w -c probe.c -o g.o
arm-none-eabi-objdump -d t.o g.o
```

**Caveat that shaped this plan:** `regression_disasm.py` counts *instructions*,
not bytes, and only compares functions with global (`T`) linkage present in both
objects. Thumb-2 mixes 16- and 32-bit encodings, so instruction parity is not
size parity. Byte-level checks were run separately (`arm-none-eabi-size`) and
are reported alongside every instruction figure below.

---

## 2. Baseline

### 2.1 Instructions, whole corpus

| | |
|---|---|
| Tests compiled | 4151 |
| Functions compared | 20435 |
| TCC / GCC instructions | 696,942 / 648,284 = **1.08x** |
| Functions where TCC is *better* | 7839 (−80,811 instr) |
| Functions where TCC is *worse* | 9748 (+129,446 instr) |

Per suite:

| suite | funcs | TCC | GCC | ratio |
|---|---|---|---|---|
| ir | 637 | 61,774 | 30,027 | **2.06x** |
| bug | 67 | 3,449 | 2,513 | 1.37x |
| pic-tds | 19 | 562 | 401 | 1.40x |
| float | 115 | 5,364 | 4,290 | 1.25x |
| gcc-compile | 3084 | 74,543 | 60,980 | 1.22x |
| tests2 | 202 | 5,507 | 4,506 | 1.22x |
| gcc-execute | 16308 | 545,648 | 545,508 | 1.00x |

The aggregate 1.08x flatters us: it nets a large win on one side against a
large loss on the other. The **gross** excess (+129,446) is the number the
levers below attack.

### 2.2 Bytes, and where the excess concentrates

Over the first 250 `gcc.c-torture/execute` tests, restricted to global
functions present in both objects:

| | instructions | bytes | 32-bit-encoding share |
|---|---|---|---|
| TCC | 10,961 | 29,346 | 33.9% |
| GCC | 6,771 | 17,824 | 31.6% |
| ratio | 1.62x | **1.65x** | — |

Bytes track instructions almost exactly, and the wide-encoding *share* is
nearly identical. **Encoding-width selection is not the systemic gap** — with
two bounded exceptions, branches (§3.4) and `mul` (§3.8), which together account
for essentially all of the recoverable width. The gap is instruction count.

Excess is concentrated but not in a handful of functions:

| bucket | share of gross excess |
|---|---|
| top 100 worst functions | 32% |
| top 500 | 56% |
| top 2000 | 78% |
| **`main` alone (1387 functions)** | **47% (+60,436)** |

### 2.3 Cycles

`metrics/qemu_corpus.py --opt=-O2 --suite ir`: 381 timed tests,
**13,866,504 cycles** total. This is the speed baseline to diff against.

### 2.4 Real embedded code (bytes of `.text`, `-O2`)

| test | TCC | GCC | ratio |
|---|---|---|---|
| `bench_bubble_sort` | 213 | 121 | 1.76x |
| `mibench_bitcount` | 322 | 186 | 1.73x |
| `bench_matrix_mul` | 304 | 192 | 1.58x |
| `bench_binary_search` | 199 | 151 | 1.32x |
| `bench_array_sum` | 127 | 95 | 1.34x |
| `mibench_dijkstra` | 706 | 558 | 1.27x |
| `mibench_stringsearch` | 1298 | 1127 | 1.15x |
| `mibench_sha` | 1249 | 1129 | 1.11x |
| `mibench_qsort` | 707 | 672 | 1.05x |
| `mibench_rijndael` | 30,842 | 29,138 | 1.06x |

The loop-heavy kernels are the worst. That is not a coincidence — see §3.1.

Two rows are omitted from the table because they measure GCC's unrolling
appetite rather than our codegen, and TCC wins both by a wide margin:
`mibench_crc32` 195 B vs 1215, `bench_fibonacci` 143 B vs 743. Aggressive
unrolling is not a gap we want to close.

---

## 3. The levers

Ranked by measured corpus impact ÷ implementation risk. Each lever names the
files to change and the evidence that justifies it.

| § | lever | measured yield | risk |
|---|---|---|---|
| 3.4 | forward branch narrowing | 3214 B = 2.51% `.text` | med |
| 3.2 | constant-size block copies | 259 call sites in 385 files; size + cycles | low-med |
| 3.1 | loop rotation (remaining shapes) | ~800–1100 B; **~12–25% cycles on tight loops** | med |
| 3.4 | `cbz`/`cbnz` | +588 B after narrowing | low, gated on narrowing |
| 3.7 | switch lowering (6 sub-levers) | 33–75% on switch-heavy functions | low→high |
| 3.8 | `mul.w` → `muls` | 336 B (168 of 263 sites) | very low |
| 3.6 | 64-bit lowering | 3 of the 10 worst non-`main` functions | med |
| 3.3 | bit-manipulation builtins | small on corpus, large on firmware | low |
| 3.5 | inlining gate widening | 47% of gross corpus excess (see caveat) | high |

### 3.1 Loop rotation — bottom-testing every counted loop

**Evidence.** TCC emits the un-rotated shape for ordinary counted and
`while` loops: a forward *conditional* branch out plus an *unconditional*
back-branch, i.e. two branches per iteration.

```c
int sumloop(const int*p,int n){int s=0;for(int i=0;i<n;i++)s+=p[i];return s;}
```

```
TCC                                    GCC
  movs r2,#0                             cmp   r1,#0
  movs r3,#0                             mov   r3,r0
top:                                     mov.w r0,#0
  cmp  r3,r1                             ble.n exit
  bge.w exit         <- 4 bytes          subs  r3,#4
  ldr.w ip,[r0,r3,lsl #2]                add.w r1,r3,r1,lsl #2
  add  r2,ip                           top:
  adds r3,#1                             ldr.w r2,[r3,#4]!
  b.n  top           <- extra branch     cmp   r3,r1
exit:                                    add   r0,r2
  mov  r0,r2                             bne.n top
  bx   lr                                bx    lr
```

Same shape in `pop`, `strl`, `f(int*,int)` and in every loop of the mibench
kernels in §2.4 — which is why those are the worst real-code rows.

**Scale.** Over the first 300 torture tests (290 compile clean, 116,562 B of
`.text`) there are 1096 backward branches: 900 already bottom-tested, 196
unconditional back-edges, of which **176 carry the un-rotated shape** across
58 of 290 files. A/B with `TCC_DISABLE_PASS=ssa:loop_rotate` shows the existing
pass already buys 1158 B (0.98% of `.text`, ~132 loops). Closing the remaining
176 is worth roughly a second helping of that: ~350 B directly, ~800–1100 B
(0.7–0.9%) once the guard branches narrow and trampoline jumps disappear.

The **cycle** win is the real prize. On M33 (no branch predictor, refill ≈ 2):
un-rotated loop control is `cmp`(1) + not-taken `b<cc>`(1) + taken `b`(3) =
5 cycles/iteration; rotated is `cmp`(1) + taken `b<cc>`(3) = 4. That is
**~12% on a three-instruction body** like `sumloop`, and ~25% with the
pointer-bump form below.

**Why it does not fire.** The pass is
`ssa_opt_loop_rotate` ([source/opt/ssa/loop/loop_rotate.c:69](../../source/opt/ssa/loop/loop_rotate.c#L69))
over `try_rotate_loop`
([source/opt/flat/loop/loop_rotate.c](../../source/opt/flat/loop/loop_rotate.c)),
run from [ir/regalloc.c:4424](../../ir/regalloc.c#L4424) — *after* the whole
flat pipeline. Guard synthesis is **not** the gap: the pass already leaves the
original header `CMP`/`JUMPIF` in place to serve as the pre-loop guard and
writes a new bottom test, with no trip-count ≥ 1 requirement. Shape coverage
is the gap. Today only `for` loops with a register-only, single-carried-variable
body rotate. The rejecting guards, each confirmed by isolation probe:

| blocker | guard | share of the 176 |
|---|---|---|
| body contains a load/store | `loop_rotate.c:359-363` (`LOAD_INDEXED`/`STORE_INDEXED`) and `:364-372` (indirect lvalue) | 148 (84%) |
| body contains an inner branch | `loop_rotate.c:441-482`, `:490-497` | 127 (72%) |
| body contains a call | `loop_rotate.c:354-358` | 105 (60%) |
| `while` / `TEST_ZERO` header (no `CMP`, no trampoline `JUMP`) | `loop_rotate.c:34`, `:38` — mirrored in `loop_relayout.c:157-159` | 16 (9%) |
| more than one loop-carried def | `loop_rotate.c:339-343` | — |

A structural constraint bounds what can be fixed in place: the rewrite
overwrites only the freed header region and rejects anything that needs to grow
the IR (`loop_rotate.c:523-528`). A `while` loop has no spare trampoline `JUMP`
to consume, so **`while` support requires a new IR-growing, guard-synthesizing
transform** — it cannot be reached by relaxing guards.

Note when instrumenting: the `--dump-ir` banner `=== IR AFTER LOOP ROTATION ===`
is printed at [source/opt/function_pipeline.c:231](../../source/opt/function_pipeline.c#L231),
*before* rotation actually runs. It is mislabelled; do not trust it.

**Pointer-bump / writeback addressing.** `TCCIR_OP_LOAD_POSTINC` and
`STORE_POSTINC` exist ([tccir.h:79](../../tccir.h#L79)), have operand config,
codegen dispatch ([ir/codegen.c:3721](../../ir/codegen.c#L3721)) and a working
backend emitter ([arm-thumb-gen.c:8352](../../source/backend/arch/arm/thumb/arm-thumb-gen.c#L8352)) —
and **no pass in the compiler ever writes them**. Corpus count of writeback
loads/stores: GCC 137, TCC 3 (all from a block-copy helper, none from loops).
`iv_strength_reduction.c:132-137` explicitly skips uses that are already
`LOAD_INDEXED`/`STORE_INDEXED`, and the flat pipeline fuses every array loop
into that form before IV-SR runs — so array loops are skipped by construction.
`decrement_to_zero.c:72` requires a literal loop limit, which excludes every
`i<n` loop.

**Work, in order:** (1) relax the body memory guards in `try_rotate_loop`
(unlocks 84% of the remaining loops including `sumloop`; the operand
save/restore at `:600-610` already carries the extra pool slot, so the `:359`
guard looks vestigial); (2) relax `ncarried_defs > 1`; (3) emit
`LOAD_POSTINC`/`STORE_POSTINC`, either by dropping the IV-SR indexed skip or as
a standalone latch peephole; (4) symbolic limits in `decrement_to_zero`;
(5) the new guard-synthesizing rotate for `while`/`TEST_ZERO` headers.

**Risk:** medium for (1)–(4), high for (5). The comment at `loop_rotate.c:276`
cites a downstream mishandling behind the memory guard — a fuzz sweep
(`scripts/diff_olevels.py`) is mandatory before trusting the relaxation.

### 3.2 Constant-size block copies — reach the LDM/STM path that already exists

**Evidence.** Across 385 `gcc.c-torture/execute` tests TCC emits **265 runtime
`mem*` calls, 259 of them with a compile-time-constant size** (histogram:
6→19, 8→30, 9→17, 10→42, 12→40, 16→64, rest larger). GCC emits **24** calls
on the same corpus, 12 with constant size.

```c
struct T{char a; int b; short c;};
void h(struct T*d, struct T*s){ *d=*s; }        /* 12 bytes */
```

```
TCC                                    GCC
  push  {r3,lr}                          mov     r3,r0
  movs  r2,#12                           ldmia   r1,{r0,r1,r2}
  bl    __aeabi_memmove4                 stmia.w r3,{r0,r1,r2}
  pop   {r3,pc}                          bx      lr
```

Corpus-wide TCC emits 130 `ldm/stm` against GCC's 405.

**The machinery is already there.** `TCCIR_OP_BLOCK_COPY` is lowered to an
inline LDM/STM sequence below `TCCIR_BLOCK_COPY_MEMCPY_MIN_BYTES` (64) in
[arm-thumb-gen.c:12597](../../source/backend/arch/arm/thumb/arm-thumb-gen.c#L12597).
But that opcode only accepts **a rodata SYMREF source and a stack-frame-offset
destination** — it was built for the const-array-template work. Ordinary
struct assignment never reaches it: `tccgen.c` lowers it with
`vpush_helper_func(TOK_memmove4)` at
[tccgen.c:11829](../../tccgen.c#L11829), [tccgen.c:12316](../../tccgen.c#L12316)
and [tccgen.c:24665](../../tccgen.c#L24665).

**There is even a dead hook waiting for it.** [tccgen.c:12296](../../tccgen.c#L12296)
reads `#ifdef TCC_TARGET_NATIVE_STRUCT_COPY … gen_struct_copy(size); … #else`
*fall back to the helper call*. Neither the macro nor `gen_struct_copy` is
defined anywhere in the tree — the helper call is the only path that has ever
run. That `#ifdef` is the natural insertion point.

**Work:** widen `TCCIR_OP_BLOCK_COPY` (or add a sibling op) to
register-base → register-base with a constant size, then route through it:
(a) struct assignment / by-value struct argument copies, (b)
`__builtin_memcpy`/`memmove`/`memset` with constant size, (c) small aggregate
initialisation. Keep the ≥64-byte call fallback. Overlap semantics: `memmove`
requires either a non-overlap proof or a load-all-then-store-all schedule,
which the LDM/STM form gives for free up to the register budget.

**Payoff:** ~4 instructions and one call (plus its `push`/`pop` and its
register-clobber pressure) per site; 259 sites in 385 files. Both size and
cycles, and it is the most embedded-relevant lever in this document.

**Risk:** low-medium. The emitter exists and is tested; the risk is in the
opcode's operand widening and in alias/overlap correctness.

### 3.3 Bit-manipulation builtins — emit the instruction, not a libcall

**Evidence.**

| source | TCC | GCC |
|---|---|---|
| `__builtin_clz(x)` | `b.w __clzsi2` | `clz r0,r0` |
| `__builtin_ctz(x)` | `b.w __ctzsi2` | `rbit`+`clz` |
| `__builtin_bswap32(x)` | `b.w __bswapsi2` | `rev r0,r0` |
| `__builtin_bswap16(x)` | `push`/`uxth`/`bl __bswapsi2`/`ubfx`/`pop` | `rev16`+`uxth` |
| `__builtin_popcount(x)` | `b.w __popcountsi2` | `bl __popcountsi2` (same) |

All of these lower unconditionally to helper names in
[tccgen.c:21041](../../tccgen.c#L21041) (`clz`/`ctz`/`popcount`/`parity`/`ffs`)
and [tccgen.c:19191](../../tccgen.c#L19191) (`bswap`); the constant-argument
case is folded, the variable case is always a call.

**The encoders already exist and are unused by codegen:** `th_clz`
([thop_system.c:253](../../source/backend/arch/arm/thumb/thop_system.c#L253)),
`th_rev`/`th_rev16`/`th_revsh`
([thop_rev.h](../../source/backend/arch/arm/thumb/thop_rev.h)), plus `rbit`.
`grep` over `arm-thumb-gen.c` finds no emission site for any of them.

**Work:** add IR opcodes (or reuse a generic unary-op with a sub-selector) for
CLZ / RBIT / REV / REV16, lower the builtins to them in tccgen, map them in the
Thumb generator, and derive `ctz = clz(rbit(x))` and `ffs` from those. Keep the
libcall fallback for `popcount`/`parity` (M33 has no instruction).

**Payoff:** small on the torture corpus, large on real firmware (CRC,
endianness, bitmap scans, allocator free-list scans). Removes a call *and* the
helper object from the link.

**Risk:** low. Self-contained, guarded by new IR tests.

### 3.4 Branch encoding: narrowing and `cbz`/`cbnz`

**Evidence.** Over 400 tests, TCC emits **1383 wide conditional branches whose
displacement fits the 16-bit encoding** (±254) and **224 wide `b.w` that fit
the narrow ±2046 range** — 3214 wasted bytes, **2.65% of `.text`**. GCC emits
`beq.w` 0 times and `b.w` once on the same corpus; TCC emits `beq.w` 248,
`bne.w` 260, `b.w` 141, `bge.w` 58, `bcc.w` 33, `blt.w` 32.

```c
int clampv(int a){ if(a>255) a=255; if(a<0) a=0; return a; }
```
→ TCC emits `f340 8001  ble.w <+8>` for a target **4 bytes away**.

Separately, TCC emits **zero `cbz`/`cbnz`** corpus-wide (GCC: 105), and there
are **316 `cmp rN,#0` + `beq`/`bne` pairs with a forward target within +126
bytes** that qualify — each saves the compare *and* 2 bytes of branch.

**Root cause — one function.** Branch width is decided in exactly two places,
`tcc_gen_machine_jump_mop` ([arm-thumb-gen.c:12136](../../source/backend/arch/arm/thumb/arm-thumb-gen.c#L12136))
and `tcc_gen_machine_conditional_jump_mop` ([:12159](../../source/backend/arch/arm/thumb/arm-thumb-gen.c#L12159)).
Both consult `can_narrow_backward_branch`
([:12104](../../source/backend/arch/arm/thumb/arm-thumb-gen.c#L12104)), which
**hard-rejects every branch with `offset >= 0`**. Forward branches are never
narrow *by construction*. That is the entire gap.

This is a *decision* problem, not an *encoding* problem: `th_b_t1`, `th_b_t2`,
`th_b_t3`, `th_b_t4` and `th_cbz` all exist in `thop_branch.c`, and
`th_patch_call` ([:3254](../../source/backend/arch/arm/thumb/arm-thumb-gen.c#L3254))
already patches all five forms.

**Two things stand in the way.**

1. A full span-dependent relaxation engine already exists — `BranchOptState` /
   `branch_opt_analyze`, [arm-thumb-gen.c:1044-1226](../../source/backend/arch/arm/thumb/arm-thumb-gen.c#L1044) —
   with correct iterative-fixpoint logic and a `TCC_LOG_BRANCH_OPT` counter.
   **It is dead code**: disabled at `:1098`, and nothing ever appends to its
   branch list.
2. Most functions **never run a dry pass at all**. `can_skip_dry_run`
   ([ir/codegen.c:2124](../../ir/codegen.c#L2124)) skips it whenever the dirty
   register count is low, which covers almost every function in the corpus. Its
   own comment concedes "falls back to 32-bit encodings for all branches
   (2 bytes wasted per branch; acceptable tradeoff)".

**A fixpoint is not needed.** Simulating relaxation over the real corpus:
single-shot narrowing gets 1607 branches / 3214 bytes; iterating to a fixpoint
gets 1608 / 3216. **One pass captures 99.94% of the win.** Design for one dry
pass plus one real pass and drop the convergence machinery.

**The hazard is literal-pool flush points**, and it has bitten before. Flushes
trigger on a running byte count (`th_literal_pool_generate` :2504,
`th_literal_pool_would_flush_for` :2730), so narrowing shifts them; a flush that
migrates to sit *between* a committed narrow branch and its target inserts up to
~1 KB and pushes it out of range, where `th_patch_call` cannot widen in place
and errors out. Prior art in-tree: `tests/ir_tests/207_fuzz_literal_pool_branch_narrowing.c`
(the guard added for it is the `th_literal_pool_would_flush_for(2)` check at
`arm-thumb-gen.c:12130`), `254_fuzz_it_block_literal_pool_flush.c`, and
`bug_cbz_far_zero_branch.c`.

**Sound design:** record pool flush points during the dry pass and *replay
exactly those* in the real pass. With flush points pinned, every remaining
real-vs-dry delta is a shrink, so a shrink between a forward branch and its
target can only reduce the distance — `dry_distance ≤ 254 ⟹ real_distance ≤ 254`
becomes a theorem. Repeating *dry* passes is free (they emit no bytes and no
relocations); repeating the *real* pass is not, because relocations are appended
during it.

**`cbz`/`cbnz` is gated on exactly this.** The whole pipeline exists — peepholes
at [ir/codegen.c:2579](../../ir/codegen.c#L2579) and [:2818](../../ir/codegen.c#L2818),
emitter `tcc_gen_machine_cbz_jump_mop` (`arm-thumb-gen.c:12194`), patcher
(`:3294`) — and is switched off by `const int cbz_enabled = 0;` at
[ir/codegen.c:2373](../../ir/codegen.c#L2373). The 20-line comment above it
explains that both distance estimators were unsound and prescribes: *"Re-enable
only behind a proper iterative branch-relaxation pass that re-emits
out-of-range CBZ candidates as wide."* That is this lever. Standalone worth
1176 bytes; **+588 incremental** once forward narrowing lands.

**Payoff:** 3214 B (2.51%) for forward narrowing, +588 B for `cbz`.
**Risk:** medium — no new encoders, but pool-flush pinning must be proven, and
the fuzz sweep is mandatory.

### 3.5 Inlining and interprocedural constant propagation

**Evidence.** `main` accounts for **47% of the gross instruction excess**
(+60,436 across 1387 functions). The mechanism is uniform — e.g.
`gcc.c-torture/execute/20020506-1.c` calls eight helpers 32 times with
constant arguments; GCC inlines, folds every branch, proves every `abort()`
unreachable, and emits `main` as 2 instructions. TCC emits 107.

TCC's inliner is a **parser-level token-replay** mechanism (`func_auto_inline`,
`func_eval_only_inline`, `auto_inline_sig_ok`
[tccgen.c:7769](../../tccgen.c#L7769), registration at
[tccgen.c:29961](../../tccgen.c#L29961), call-site expansion at
[tccgen.c:17240](../../tccgen.c#L17240)), plus a post-optimisation re-decision
in [source/backend/generators/function.c:388-491](../../source/backend/generators/function.c#L388).
An `-fipc` interprocedural-constant-propagation flag exists but operates only
on `const_call_fold.c`'s scalar result cache.

**Honest framing:** a test-driver `main` calling helpers with literal constants
is not representative of firmware, so the +60,436 figure will not translate
one-for-one to real code. The same lever in real code is "inline small static
helpers and specialise on constant arguments" — `bench_function_calls` is 120
bytes against GCC's 96. Sell it on that, not on the torture number.

**Two stale beliefs, both disproved by measurement.** `for` loops are *not*
rejected: `inline_body_has_unsafe_loops`
([tccgen.c:8268](../../tccgen.c#L8268)) rejects a loop only when the innermost
open bracket is `(` or `[`; the doc comment above it is out of date, and
`int t(int c){int s=0;for(int i=0;i<c;i++)s+=i;return s;}` inlines at -O2. And
the token budget is *not* the binding constraint at the sizes that matter.

**What actually blocks `20020506-1`**, established by bisection (truncating
`test1` to one of its four blocks makes `main` collapse to the GCC-identical
2 instructions):

1. **The post-opt call-heavy demote**, `call_ops >= 3` at
   [function.c:469](../../source/backend/generators/function.c#L469). `test1`
   compiles standalone to five `bl abort`, so its `func_auto_inline` is
   revoked. This fires on the *callee's stand-alone* IR — precisely the case
   where constant arguments would have deleted every one of those calls.
   `-finline-limit=400` does not restore inlining, proving this, not the token
   budget, is binding.
2. **`inline_body_has_side_effects`** ([tccgen.c:8049](../../tccgen.c#L8049))
   treats any `<ident> (` at brace depth 1 as a side effect, so a callee
   containing a top-level call can *never* reach the constant-args
   (`eval_only`) path. Every assertion-style helper in the corpus trips this.
3. **`void_llong_limit = 15` words** ([tccgen.c:30013](../../tccgen.c#L30013))
   for void-return-plus-`long long`-param signatures kills `test7`/`test8` and
   every `long long` torture test outright.
4. **Tokens are freed** at [tccgen.c:30197-30203](../../tccgen.c#L30197) once a
   function neither promotes nor qualifies for late re-opt, so by the time
   `main` is parsed there is no `InlineFunc` left to expand even on constant
   arguments.

Footgun worth fixing while in there: `body_len` counts token-stream *words*
including `TOK_LINENUM`, so **reformatting a function across more source lines
can make it stop inlining.** Count real tokens.

**Clarification on `func_eval_only_inline`:** it is the "keep the tokens alive"
bit. The constant-argument restriction lives in the separate predicate
`eval_only_all_const` ([tccgen.c:17198](../../tccgen.c#L17198)), which requires
every user-visible argument to be `VT_CONST`.

**Architecture.** Compilation is strictly single-pass, emit-as-you-parse:
`gen_function` allocates fresh IR, builds it during `block(0)`, optimises,
allocates registers, emits machine code, and frees the IR — all inline in
`decl()`. Callee IR *can* outlive its function, but only through
`ir_inline_stash` ([function.c:692-733](../../source/backend/generators/function.c#L692)),
which is limited to `static`, non-address-taken, ≤200-op functions and — the
sticking point — snapshots **after** register allocation and codegen, so vregs
are already bound to physical registers and locals to frame offsets. Its own
comment says *"Today there are no consumers — this exists to validate the
lifecycle change."* Whole-TU re-processing is nonetheless architecturally
accepted already: `gen_late_reopt_functions`
([tccgen.c:29231](../../tccgen.c#L29231)) re-compiles a function at end of TU
and erases its old `.text` range.

**Recommendation: widen the gates first (design (a)).** The corpus is not
blocked by anything token replay fundamentally cannot do — the truncated-body
experiment already produces GCC-identical output. Four localized changes cover
the dominant pattern: make the `call_ops >= 3` demote call-site-aware (or stop
counting calls to `noreturn` callees); whitelist `noreturn` calls in the
side-effect rule, or replace that lexical heuristic with the `TuFuncSummary`
purity data that is already computed; raise `void_llong_limit`; retain tokens
when any caller in the TU passes all-constant arguments.

**Design (b), an IR-level inliner + IPA const-prop**, remains the right
long-term target and its first phase is already paid for (`ir_inline_stash`,
the `source/opt/flat/ipa/` summaries — a real intra-TU call graph, purity,
per-function pointer-write ranges — and the late-reopt re-emit path). It needs
the stash moved to a *pre-RA* snapshot and a vreg/frame-offset renumbering
splice. Multi-week, and a much wider miscompilation surface.

**Top hazards for (a):** compile-time and memory blowup (the eval-only path
expands the whole body at every constant-arg site *before* const-prop and DCE
run — 32 × ~200-word bodies for `20020506-1` alone; the `inline_count >= 8`
budget only covers call-heavy callees, so a global expansion budget is needed);
token-replay rebinding hazards, which every existing lexical guard exists to
catch; and code-size regression on non-static callees, which are always also
emitted standalone, so inlining them is pure duplication absent
`--gc-sections` — that is what the `nonstatic_bloat` rule at `function.c:467`
protects.

**Risk:** high, and the largest single work item here. Sequence it last.

### 3.6 64-bit integer lowering

**Evidence.** The simplest `long long` operations carry a fixed overhead — the
result pair is computed in callee-saved registers and then copied to r0:r1,
forcing a `push`/`pop` that the function otherwise would not need.

| source | TCC | GCC |
|---|---|---|
| `a+b` | 7 instr / 16 B (`push {r4,r5}`, compute in r4:r5, `mov r0,r4`, `mov r1,r5`, `pop`) | 3 / 8 |
| `a&b` | 7 / 18 (wide `and.w` where narrow `ands` fits) | 3 / 8 |
| `a<b` | 6 (`push {r0}` … `pop {r0}` for nothing) | 5 |
| `-a` | 10 (materialises two zero registers, pushes lr) | 3 |
| `a*b` | 22 (`stmdb {r4,r5,r8,r9}`, 8 redundant `mov`, 2 dead `movs`) | 5 |
| `a<<n` | `b.w __aeabi_llsl` | 8 inline |

This shows up in the corpus as `ashrdi-1::constant_shift` (709 vs 224),
`gcc-compile/950612-1::f` (926 vs 274) and `arith-rand-ll::main` (1288 vs 131).

**Work:** bias the destination of a function's 64-bit result pair to r0:r1 in
the register allocator (prior art exists — the pr58574 soft-float work added a
return-pair r0:r1 preference; it evidently does not cover the plain integer
path); prefer narrow encodings for 64-bit ALU halves when both operands are
low registers; drop the spurious `push`/`pop` around 64-bit compares; lower
`neg64` to `negs`+`sbc`; consider inlining variable 64-bit shifts.

**Risk:** medium. Register-allocator preference changes have historically
caused wide, hard-to-attribute regressions — land each sub-lever separately
behind its own measurement.

### 3.7 Switch lowering

**Evidence.** A dense 6-case switch costs TCC **96 bytes** against GCC's 12:

```
TCC                                    GCC
  push {r4,r5}          <- why?          cmp   r0,#5
  mov  r4,r0                             itte  ls
  b.w  dispatch                          lslls r0,r0,#1
  ...                                    addls r0,#3
  case bodies, each ending               movhi r0,#0
  in a 4-byte b.w to a shared            bx    lr
  epilogue
  ...
dispatch:
  mov  r5,r4            <- redundant
  cmp  r5,#5
  bhi.n default
  <PC-relative table, 4 bytes/entry>
```

Measured across purpose-built shapes (`.text` bytes, TCC vs GCC): 6 dense
`return K` cases 98/12; 6 dense `f(); break;` cases 104/40; 8 dense cases on
`*s` 122/20; 12 contiguous `case a ... b` ranges 152/24; a 60-case 50%-dense
switch **910**/20+238 rodata; `118_switch.c` 2736/728.

Two corrections to the obvious hypotheses, both verified:

- **`gcase()` is not a linear chain.** It splits binarily once `len > 8`
  ([tccgen.c:25165](../../tccgen.c#L25165)) and `118_switch`'s disassembly shows
  a genuine decision tree. Tree shape is not the 3.76x.
- **Case ranges are lowered as range compares**, not expanded per value
  (`case_sort` [tccgen.c:24971](../../tccgen.c#L24971), `gcase`
  [tccgen.c:25184](../../tccgen.c#L25184)). Correct and compact already.

The actual costs, in order:

1. **Dispatch-after-body layout.** `tccgen.c:26075` emits a forward `JUMP` over
   the case bodies to a dispatch block placed *after* them. That single choice
   produces all three artefacts: the leading always-wide `b.w` (4 B); a switch
   operand live from function entry across the entire body, which linear scan
   must give a **callee-saved** register (`push {r4,r5}` + `mov r4,r0`, 6 B);
   and, at [tccgen.c:26098](../../tccgen.c#L26098), a *second* copy into a fresh
   temp vreg that is never coalesced (`mov r5,r4`).
2. **Scratch-spill storm on 64-bit switches.** In `118_switch.c`, **408
   push/pop instructions = 896 of 2736 bytes (33%)**; `ibdg` alone has 101
   pushes and 101 pops against GCC's one each. Isolating the same comparisons
   as an `if`-chain instead of a `switch` turns 6 spill instructions back into
   2 — the redundant copy above pins r0/r1 to a dead value exactly when a
   64-bit literal compare needs two scratch registers. `lr` is then push/popped
   *per comparison site* rather than once in the prologue.
3. **No `tbb`/`tbh`.** `th_tbb` exists
   ([thop_tbb.c:67](../../source/backend/arch/arm/thumb/thop_tbb.c#L67)), with
   the `tbb_tbh` feature bit set for every v8-M core, and its only caller is the
   inline assembler. Codegen instead emits a 14-byte preamble plus a
   **4-byte-per-entry** PC-relative table
   ([arm-thumb-gen.c:3428](../../source/backend/arch/arm/thumb/arm-thumb-gen.c#L3428)),
   where GCC uses `tbb [pc,r0]` — 4 bytes plus 1 byte per entry.
4. **A jump-table heuristic calibrated for the wrong entry width.**
   `switch_can_use_jump_table` ([tccgen.c:25013](../../tccgen.c#L25013))
   requires only 50% density (`n*2 < range`) and caps `range > 65536` — with
   4-byte entries that admits a 256 KiB `.text` table. The 60-case test above
   passes at exactly 50% and produces a **476-byte in-`.text` table**. It also
   rejects tables outright for any case *range* (`:25037`) and for any
   `long long` switch (`:25044`), even when all values fit in 32 bits.
5. **`switch_to_data` misses the commonest shape.** It only matches case bodies
   of the form `ASSIGN dst <- const; JMP merge`
   ([switch_to_data.c:19-97](../../source/opt/flat/cfg/switch_to_data.c#L19)),
   so `return K` bodies (`RETURNVALUE`) never reach it — which is why the
   `return K` variant costs 98 B and the `r=K; break;` variant costs 36 B for
   the same switch. Its data table is also hard-coded to 4 bytes/entry
   (`:220`), where GCC narrows to 2 or 1.

**Work, cheapest first:** retune the admission heuristic (§4 above — absolute
table-byte cap, density as a function of entry width, drop the meaningless
65536 cap); kill the redundant operand copy at `tccgen.c:26098`; admit case
ranges into the table by filling the whole `v1..v2` span; then `tbb`/`tbh`;
then relayout the dispatch block ahead of the bodies (mirroring
`loop_relayout.c`); then narrow `switch_to_data` element width and teach it
`RETURNVALUE` bodies; then 64-bit switches whose cases fit in 32 bits, behind a
`hi == sign_extend(lo)` guard. The scratch-spill hoist (item 2) is a
register-allocator change and belongs with §3.6.

**Risk:** low for the heuristic and copy fixes, medium for `tbb` (two-pass size
agreement plus table alignment) and relayout, high for the spill hoist. Note
that `tbb` and relayout both change code size between the dry and real passes —
the same two-pass contract that §3.4 has to make sound. Signed/unsigned edge
cases in `case_cmp` ([tccgen.c:24956](../../tccgen.c#L24956)) are where this
area has historically broken; gate on `diff_olevels.py` and `make test-all`.

### 3.8 Smaller, self-contained idioms

| idiom | TCC | GCC | where |
|---|---|---|---|
| `mul.w rd,rd,rm` | wide, 268 sites | `muls rd,rm` (T16) | `thop_mul.c:107` |
| narrow ALU inside IT blocks | `movne.w r0,#1`, `rsblt r0,r0,#0` | `movne r0,#1`, `neglt r0,r0` | `thumb.h:645`, `:655` |
| signed `x/8`, `x%8` | `sdiv`+`mul`+`sub` (2–12 cyc) | `cmp`/`addlt`/`asrs` | `source/opt/ssa/scalar/strength.c` |
| `x/C`, `x%C` for constant C | hardware `sdiv`/`udiv` (327 sites vs GCC's 59) | multiply-high reciprocal | strength reduction |
| saturating clamp | branchy `cmp`/`b.w`/`mov` | `ssat` / `usat` | new peephole |
| nested `?:` max/min | 9 instr, two `b.w`, a join `mov` | 6 instr, flat IT chain | `source/opt/flat/cfg/if_convert.c` |
| `for(i)p[i]=0` | open-coded loop | `memset` call | loop idiom recognition |

The division row is a **speed** lever with a **size** cost (the reciprocal
constant needs a literal-pool word): M33's `SDIV` is 2–12 cycles and
data-dependent, `UMULL` is 1. It should be gated on the optimisation goal, not
applied unconditionally.

The first two rows are quantified and near-free:

- **`mul`** — `th_mul` ([thop_mul.c:107](../../source/backend/arch/arm/thumb/thop_mul.c#L107))
  takes the T16 `MULS Rdm,Rn,Rdm` path only when `rd == rm`. MUL is
  commutative, so `mul.w r3,r3,r2` is `muls r3,r2`. **168 of 268 `mul.w` in the
  corpus have this shape = 336 bytes.** The fix must gate T16 on
  `flags != FLAGS_BEHAVIOUR_BLOCK` — `th_mul` currently does `(void)flags`,
  which is a latent flag-clobber bug worth fixing regardless.
- **IT blocks** — `thumb.h:645` and `:655` reject every T16 `implicit_s` shape
  inside an IT block. That is architecturally wrong for ARMv8-M: in a 16-bit
  data-processing encoding the S bit is `!InITBlock`, so `movne r0,#1` and
  `neglt r0,r0` are 2 bytes inside IT and do not touch NZCV (confirmed against
  `arm-none-eabi-as -mcpu=cortex-m33`). `mov_reg_t1_shift_emit`
  ([thop_mov.c:42](../../source/backend/arch/arm/thumb/thop_mov.c#L42)) already
  encodes the rule correctly, so those two blanket rejections are the outlier.
  Payoff is small (~68 bytes: 48 wide instructions inside IT blocks corpus-wide,
  ~34 narrowable) but the fix is principled.

---

## 4. Sequencing

Each phase is independently shippable and independently measurable. Do not
start the next until the previous is green on
`python3 metrics/compare_worktree.py` (correctness + size + cycles in one run).

| phase | levers | expected | risk |
|---|---|---|---|
| **P1** | §3.8 `mul.w`→`muls`; §3.7 items 4+5 (jump-table heuristic, redundant switch copy); §3.3 bit builtins | 336 B + switch blowup fix + firmware win | low |
| **P2** | §3.2 constant-size block copies via the dead `gen_struct_copy` hook | 259 call sites; size + cycles | low-med |
| **P3** | §3.4 forward branch narrowing (pool-flush pinning, one dry + one real pass), then `cbz`/`cbnz` | 3214 + 588 B ≈ 3% `.text` | med |
| **P4** | §3.1 loop rotation: relax body memory guards, then `ncarried_defs`, then `LOAD_POSTINC` | largest cycle win | med |
| **P5** | §3.6 64-bit lowering; §3.7 `tbb`/`tbh` + dispatch relayout; §3.8 idioms | targeted | med |
| **P6** | §3.5 widen the inline gates (call-heavy demote, side-effect rule, `long long` cap, token retention) + a global expansion budget; §3.1 guard-synthesizing `while` rotation | largest corpus number, most work | high |
| **P7** | §3.5 design (b): pre-RA IR stash + IR-level inliner + IPA const-prop | higher ceiling than P6 | very high |

P1 is three independent low-risk changes and can go in parallel. **P3 must
precede `cbz`** (the code comment disabling `cbz` names branch relaxation as its
prerequisite) and should precede P5's `tbb` work, since both depend on the same
dry-run/real-run size contract being made trustworthy.

---

## 5. Verification protocol

Non-negotiable for every lever, in this order:

1. `make clean && make cross -j16` — the `loop_const_sim` incident showed that
   iterative IR-pass edits without a clean rebuild produce untrustworthy suite
   results.
2. `make test -j16` — IR suite is the primary correctness gate.
3. `python3 metrics/compare_worktree.py` — correctness verdict, `.text` bytes
   and cycles against the baseline, both sides measured in one pass.
4. `./scripts/regression_disasm.py --suite all -j16 --diff <baseline>` —
   per-function regressions, to catch a lever that wins on average while
   pessimising a shape.
5. `python3 scripts/diff_olevels.py --seeds 0-5000 --require-qemu` — fuzz
   self-consistency, before anything touching the register allocator or
   branch encoding.
6. A guard IR test per lever in `tests/ir_tests/`, registered in
   `TEST_FILES` in `tests/ir_tests/test_qemu.py`.

Record each landing with `metrics/record.py` so the dashboard shows the effect
as a graph rather than a claim.

---

## 6. Explicitly not in scope

- **Encoding-width selection in general.** Measured: TCC 33.9% wide vs GCC
  31.6% on identical functions. An audit of every 32-bit instruction in the
  corpus against its available T16 form (low registers, fitting immediate:
  `and`/`orr`/`eor`/`bic`, `add`/`sub` imm3/imm8/reg, `mov #imm8`, `cmp #imm8`,
  `ldr`/`str [rn,#imm5]` and `[sp,#imm8]`, `mvn`) found **342 recoverable
  bytes in total, 336 of which is the `mul` case in §3.8**. Outside branches
  and `mul`, `thop_*` selection is already picking T16 wherever legal.
- **Register-to-register move elimination in general.** Measured: TCC 5.25% of
  instructions vs GCC 4.53%. The 64-bit case (§3.6) is a real defect; the
  general case is not a systemic gap.
- **High-register pressure.** TCC touches r8–r12 in 7.9% of instructions
  against GCC's 5.2%, and 707 of TCC's 3950 wide instructions are wide because
  of a high register. Real, but second-order — revisit after P1–P5.
- **Instruction scheduling.** M33 is mostly in-order and single-issue; the
  return does not justify the machinery.

---

## 7. Landed

### §3.8 `mul.w` → `muls` — landed

`th_mul` ([thop_mul.c:107](../../source/backend/arch/arm/thumb/thop_mul.c#L107))
now takes the T16 path for `rd == rn` as well as `rd == rm` (MUL is
commutative — the non-destination operand plays `Rn`), and threads `flags`
into `thop_emit` instead of discarding it. Two correctness fixes come with it:
`SHAPE_MUL_T16` is marked `implicit_s`, so the flag-setting T16 form is no
longer selected when the caller needs NZCV preserved, and `muls` with three
distinct registers — not encodable on ARMv8-M — is now rejected instead of
being silently emitted as a non-flag-setting `mul.w`.

Measured over the first 400 `gcc.c-torture/execute` tests (385 compile):
268 `mul.w` → 100 `mul.w` + 168 `muls`; `.text` **131,398 → 131,096 B (−302)**.
The predicted 336 B minus ~34 B of second-order literal-pool/branch drift.
Corpus instruction counts are unchanged (this only narrows encodings).
Unit tests: `test_mul_t16_commutative_rd_eq_rn`,
`test_mul_t16_rejected_when_flags_must_be_preserved`,
`test_mul_t32_cannot_set_flags` in `tests/unit/arm/armv8m/test_thop_mul.c`.

### §3.3 Bit-manipulation builtins — landed

New IR opcodes `TCCIR_OP_CLZ / RBIT / REV / REV16` (unary, `{1,1,0}`), appended
at the end of `TccIrOp` so no existing opcode value shifts. Backend emitter
`tcc_gen_machine_bitop1_mop` drives the already-present `th_clz` / `th_rbit` /
`th_rev` / `th_rev16`. `tcc_machine_has_bit_ops()` gates the whole family on
`t32 && clz_rbit`, so v6-M and v8-M-base keep the libcalls.

| source | before | after |
|---|---|---|
| `__builtin_clz(x)` / `clzl` | `b.w __clzsi2` | `clz r0,r0` |
| `__builtin_ctz(x)` / `ctzl` | `b.w __ctzsi2` | `rbit`+`clz` |
| `__builtin_bswap32(x)` | `b.w __bswapsi2` | `rev r0,r0` |
| `__builtin_bswap16(x)` | `push`/`uxth`/`bl __bswapsi2`/`ubfx`/`pop` | `uxth`+`rev16`+`ubfx` |

`clzll`/`ctzll`, `popcount*`, `parity*` and `ffs*` keep the helper call, as
planned. Constant arguments fold in three places: directly in `tccgen`,
in `ir_opt_eval_const_u64` ([const_eval.c](../../source/opt/util/const_eval.c)),
and — **load-bearing** — in a new `fold_bitop1` gen in
[ssa/scalar/fold.c](../../source/opt/ssa/scalar/fold.c). The last one is not
optional: `bitop_const_fold` only knows how to fold the *helper calls*, so
without an equivalent rule on the new opcodes a propagated constant stays a
runtime instruction. Omitting it regressed `ir/145_builtin_bswap::main`
from 5 to 40 instructions; with it, `main` is 5 again.

Corpus effect (`regression_disasm --diff`, 20,443 functions): **14 improved,
6 "regressed", 20,421 unchanged, net −45 instructions.** All six regressions
are one-line wrapper functions where a tail-call `b.w __clzsi2` (1 instruction)
became `clz`+`bx lr` (2) — a cycle and link-size win that the instruction
metric scores backwards; see the §1 caveat. Real uses improve:
`builtin-bitops-1::main` −20, `pr37780::fooclz` −2, `pr109834-1::f` −4,
`pr101642::bar` −4. Cycles on the `ir` suite are flat (+1 over the whole
corpus once the new test's own 1,058 cycles are excluded).

Guard test `tests/ir_tests/368_native_bit_builtins.c`, registered in
`TEST_FILES`. Three pre-existing expectations were updated because they
asserted the *old* lowering: two `mul.w` mnemonic checks in
`tests/ir_tests/test_codegen_asm.py`, and the `__bswapsi2`/`__ctzsi2` symbol
assertion in `tests/runtime/test_runtime.py`, which now asserts those symbols
are absent while keeping the 64-bit and popcount helpers.

### §3.7 item 5 (first half) — redundant switch-operand copy — landed

`block()`'s `TOK_SWITCH` arm unconditionally copied the switch value into a
fresh temp vreg before the case-comparison chain (`T0 <-- P0 [ASSIGN]`), which
is the `mov r5,r4` in §3.7. It is now reused in place when it is already a
plain temp or register-parameter vreg. Three guards, each established by
measurement, not by reasoning:

- **Full register width only.** This is the important one. For a
  `char`/`short` operand the copy is what widens the value *once*; reusing the
  narrow vreg makes every node of the binary compare chain re-emit
  `mov`+`sxtb`, and it also destroys the const-prop that had collapsed
  constant-argument callers. Without this guard `pr48809::foo` was +82 and its
  `main` +38, `pr91632::foo` +16, `pr14730::t` +12. With it, all five of those
  regressions disappear. (The first hypothesis — that `tcc_ir_opt_switch_to_data`
  had stopped firing — was wrong: `pr48809` never used a jump table at all, and
  excluding the jump-table path changed nothing.)
- **No VAR vreg**, which keeps its own identity through the body; the copy is
  what pins the dispatch to the entry value.
- **`sw->n > 0`**, or the backpatch target `c` would point past the empty
  comparison chain rather than at a real landing instruction.

`switch_can_use_jump_table(sw)` is now computed once and shared.

Corpus effect (cumulative with the two levers above, vs `p1_baseline`):
**43 improved, 9 regressed, net −331 instructions.** The headline is the
64-bit switch scratch-spill storm of §3.7 item 2: `118_switch::ibdg` 445→281,
`ubdg` 451→283. Remaining regressions are the five §3.3 wrapper artefacts plus
`pr59743` +2, `va-arg-pack-1` +1/+2 and `vsnprintf-chk::test2_sub` +2.

Guard test `tests/ir_tests/369_switch_operand_reuse.c` covers the cases the
guards exist for: a body that assigns to the object being switched on,
signed/unsigned `char` and `short` operands at their range extremes, a memory
operand, a `long long` switch, and a default-only switch.

Still open in §3.7: the admission heuristic (item 4) — case *ranges* are
rejected from jump tables outright at [tccgen.c:25037](../../tccgen.c#L25037),
and a 12-range switch measures 146 B against GCC's 52 — plus `tbb`/`tbh`,
dispatch relayout, and teaching `switch_to_data` about `RETURNVALUE` bodies.

### §3.2 constant-size block copies — partially landed, premise corrected

**The §3.2 evidence does not survive re-measurement.** Re-counting the same
corpus (first 400 `gcc.c-torture/execute`, 385 compile) gives 257 runtime
`mem*` calls — but in **40 files, not 385**, and 177 of them (69%) are in three
files, `20040709-1/-2/-3`, all packed-bitfield tests. The distribution by
callee is the part that matters:

| callee | count | inlinable? |
|---|---|---|
| `__aeabi_memmove` (align 1/2) | 178 | no — byte/halfword copies cost *more* than the call |
| `__aeabi_memmove8` | 25 | yes |
| `memset` / `__aeabi_memset` | 24 | mostly runtime size |
| `__aeabi_memmove4` | 16 | yes |
| `memcpy` / `__aeabi_memcpy` | 14 | mixed |

So the addressable population is ~55 sites, not 259. And the direction of the
size claim is wrong: §3.2 asserts the inline form saves "~4 instructions and
one call" per site, but measured on the plan's own probe the call is *smaller*
— `push/movs/bl/pop` is 12 bytes against GCC's 14-byte `mov/ldmia/stmia.w/bx`.
The real payoff is **cycles** (no `bl`, no helper loop) and dropping the helper
from the link, not `.text`.

In real firmware-shaped code the remaining calls are correct as calls:
`mibench_sha`'s are 56/64-byte or runtime-size, which GCC also leaves as calls.

**What landed.** `vstore()` already had an inline expansion for
register-deref destinations, capped at 8 bytes. The cap comment cites the
pr92618 store-forwarding width-mismatch — but that hazard needs a LOCAL/GLOBAL
*source*, whose own storage an earlier differently-width store can be
mis-forwarded from. With both sides register-deref pointers the source is
opaque memory with no such store in view, so the plain `*d = *s` shape now
expands up to 16 bytes. `921012-2::f` (`struct foo *a,*b; *a=*b;`) goes from
`bl __aeabi_memmove4` to `ldr`+`ldrd`+`strd`+`str`.

Measured: **+17 instructions across 4 functions, 0 improvements** on the
corpus (which barely contains the shape), **−308 cycles** on the `ir` suite,
and roughly 3x on the idiom itself. Guard test
`tests/ir_tests/370_ptr_struct_copy_inline.c` covers self-copy and
adjacent-object copies, since the expansion is only overlap-safe because it
front-loads every LOAD before the first STORE.

**What did not, and why it is bigger than §3.2 implies.** GCC needs 4
instructions here because it uses `ldmia`/`stmia.w`; we emit individual
LDR/LDRD/STRD and need one more callee-saved register (`push {r3,r4}`), so we
land at 7. Reaching parity means actually routing through
`TCCIR_OP_BLOCK_COPY` — and that opcode is hardwired to *rodata-SYMREF source,
stack-frame-offset destination* (`tcc_gen_machine_block_copy_mop` reads
`irop_get_symref_ex` and `irop_get_imm64_ex` unconditionally). Widening it to
register-base → register-base is a new opcode plus a new emitter plus scratch
allocation for up to 4 data registers, not a matter of reaching an existing
path. The `#ifdef TCC_TARGET_NATIVE_STRUCT_COPY` hook is still the right
insertion point, but the emitter behind it has to be written.

Also still open: global/local destination ← register-deref source (`g = *s`),
which is the same hazard-free shape but needs a second store-addressing mode in
the expansion loop.

### §3.4 forward branch narrowing — landed

**The §3.4 design premise was wrong, and that is the whole story of this
lever.** The plan says "design for one dry pass plus one real pass and drop the
convergence machinery", on the assumption that the dry pass is a size model of
the real pass. It is not — it is a *discovery* pass. Where it finds no free
scratch register it merely RECORDS the push and emits nothing
(`get_scratch_reg_with_save`), and its finalisation then reassigns registers and
resizes the frame. Measured drift on `gcc.c-torture/compile/920611-2` was
**+16 bytes**, from an `OR` that changed encoding width once its register was
reassigned — enough to push a 254-byte branch to 258 and hard-error in
`th_encbranch_8`.

**The fix is a third pass.** Codegen now runs pass 0 (discovery, dry), pass 1
(**rehearsal**, dry), pass 2 (real). The rehearsal reruns the loop after
discovery has settled — same `ind`, same register assignments, same frame — and
differs from the real pass in exactly one way: every branch is emitted wide. So
the real pass is never larger, distances between any two IR instructions only
shrink, and "fits in range in the rehearsal" implies "fits for real".

Making the rehearsal faithful required splitting the two meanings that
`dry_run_state.active` had been carrying. The new `DRY_RUN_MODELLING` macro is
true only for the discovery pass ("model what would happen"); the rehearsal
takes the real code paths for scratch save/restore so its layout matches.

`can_skip_dry_run` now also requires the function to have no forward branch.
The dry passes emit no bytes, so this costs nothing measurable in compile time
(0.70s → 0.75s over 385 tests) and is what unlocks the bulk of the win.

**Three bugs had to be fixed first**, all of which the rehearsal exposed:

1. `th_literal_pool_generate` patched `cur_text_section->data + branch_pos`
   **during dry runs**, where `ind` has advanced past the allocated section —
   Valgrind: *invalid write of size 2*. Latent; one dry pass happened to stay
   inside the section.
2. The same function's `b0_prev`/`b1_prev` diagnostic **reads** had the same
   defect, and hard-SIGSEGV'd on `pr34093`.
3. **The MOV-equivalence cache leaked from the dry pass into the real pass.**
   `dry_run_init` resets it before each dry pass, but nothing reset it before
   the *real* pass, so the real pass inherited equivalences established while
   emitting *different code with different registers*. It elided a `mov r4, r0`
   that was genuinely needed — `tests2/25_quicksort::partition` produced wrong
   output at -O2. Fixed by `tcc_gen_machine_mov_coalesce_reset()` before the
   real pass, so both passes start from identical cache state (which the
   rehearsal's validity also requires). Removing this reset reproduces the
   miscompile, confirming it is load-bearing.

**Pool-flush safety.** A flush inserts up to ~1 KB, and because the real pass
emits fewer bytes a flush can only move *later* — so one sitting before the
branch in the rehearsal could drift into its range. Rather than pin flush points
(which would mean overriding the pool scheduler), `can_narrow_forward_branch`
declines whenever a flush is *possible*: the rehearsal records a running
literal-pool entry count per IR instruction, which bounds the pressure at the
target, and if that upper bound stays under the 1020-byte threshold no flush can
occur and monotonicity holds unconditionally. This is why 400 of the 3,046
recoverable bytes are left on the table.

**Results** (385 torture tests):

| | before | after |
|---|---|---|
| `.text` | 131,094 | **128,252 (−2,842, −2.17%)** |
| wide conditional branches | 1609 (1299 narrowable) | **446 (138 narrowable)** |
| wide `b.w` | 225 (224 narrowable) | **63 (62 narrowable)** |
| corpus instructions | ~696,996 | **695,057 (−1,939)** |
| `ir`-suite cycles | 13,868,125 | **13,852,053 (−16,072)** |
| compile wall (385 files) | 0.70s | 0.75s |

87% of the recoverable branch bytes, captured. The per-function instruction
diff shows 159 improved / 230 regressed, but the shape matters: improvements are
large (`pr92904::main` −923, `118_switch::ubdg` −206, `pr58574::foo` −201,
`pr82052::fn10` −111) while 226 of the 230 regressions are +1 to +4. Those come
from functions that now get a dry pass and so get different — occasionally
worse — register allocation; spot-checking `pr108783::foo` confirms they are
*not* caused by the MOV-cache fix (identical with and without it).

Guard test `tests/ir_tests/371_forward_branch_narrowing.c` stresses the
dangerous interaction directly: forward branches held live across repeated
literal pool flushes, distances swept around the T1 limit, nested conditionals
with overlapping ranges, and a forward loop exit.
`tests/ir_tests/test_codegen_asm.py::test_forward_branch_conditional_narrows`
(formerly `..._still_wide`) now asserts the opposite of what it used to.

**`cbz`/`cbnz` is now unblocked.** The 20-line comment at `ir/codegen.c` that
disables it prescribes "re-enable only behind a proper iterative branch
relaxation pass that re-emits out-of-range CBZ candidates as wide" — and names
exactly the failure mode this rehearsal pass fixes ("distances from a NO-CBZ dry
run diverge from the real layout once literal-pool flush points shift between
the passes"). `cbz_dry_mapping` is now the rehearsal's map rather than the
discovery pass's, which is the sound input it always wanted.

### §3.4 `cbz`/`cbnz` — landed

The 20-line comment disabling this asked for exactly one thing: *"Re-enable only
behind a proper iterative branch-relaxation pass that re-emits out-of-range CBZ
candidates as wide"* — and named the failure it had, *"distances from a NO-CBZ
dry run diverge from the real layout once literal-pool flush points shift
between the passes"*. The rehearsal pass is that prerequisite, so both unsound
estimators (the instructions-times-ten guess and the discovery-pass map) are
replaced by `tcc_gen_machine_cbz_forward_ok()`.

The check is stricter than plain narrowing because a committed 16-bit CBZ
**cannot be widened at patch time** — `th_patch_call` errors — so the offset has
to be bounded from *both* sides, not just above:

* **upper**: the real offset is at most the rehearsal-derived one, since the
  real pass only ever shrinks;
* **lower**: at least that minus the most the range can shrink, and it must not
  fall below 0. Each branch in the range sheds at most 4 bytes (a `CMP` + `B.W`
  pair fusing to a `CBZ`), so counting branches is a safe over-estimate.

The base offset is `dry_dist - 8`, not the `- 4` plain narrowing uses, because
the fusion itself removes 6 bytes and puts back 2.

**The fusion must be gated on `!is_dry_run`.** Left ungated it fires during the
rehearsal, which emits a 2-byte CBZ where the real pass emits 6 bytes of
`CMP` + branch — so the rehearsal *underestimates* the real layout. That is the
dangerous direction and would undermine the forward-narrowing guarantee as well.
The test suite stayed green with the bug present; it was caught by noticing only
80 fusions fired against 237 remaining eligible `cmp #0` + `beq/bne` pairs.
Gating raised it to 127.

**Results** (385 torture tests): `.text` 128,252 → **128,004 (−248)**, with
**127 `cbz`/`cbnz`** emitted where there were none. The plan predicted +588 B,
but that was measured against un-narrowed branches: forward narrowing has
already taken most of these from 4 bytes to 2, so each fusion now saves 2 rather
than 4.

`tests/ir_tests/test_codegen_asm.py::test_cbz_fusion_fires` (was
`..._disabled`) now requires the fusion and forbids the `cmp #0` + branch pair —
which is what that test's own comment always said to do once this landed.
`tests/ir_tests/371_forward_branch_narrowing.c` gained `zero_tests` and
`zero_far` for the CBZ shapes at both ends of the range.

Still conservative: 237 eligible pairs remain, most rejected by the worst-case
shrink bound assuming every branch in range collapses maximally. Tightening it
needs a per-branch shrink model rather than a bound.

### §3.1 loop rotation — step 1 landed, step 2 measured and rejected

**Step 1 (relax the body memory guard) — landed.** The plan called the
`LOAD_INDEXED`/`STORE_INDEXED` rejection vestigial and was right: the operand
save/restore already carries the 4th pool slot those ops need (`body_has_extra`),
so the rewrite reconstructs them intact. One deletion. Ordinary array loops now
bottom-test — `for (i) s += p[i]` goes from `cmp; bge exit; …; b top` to a latch
of `cmp; blt body`, one branch per iteration instead of two:

| | before | after |
|---|---|---|
| `ir`-suite cycles | 13,851,320 | **13,823,142 (−28,178)** |
| `.text`, 385 torture tests | 128,004 | 127,998 |

**A latent correctness bug found and fixed on the way.**
`write_instr_at_nop` ([loop_ir_mutate.c](../../source/opt/flat/loop/loop_ir_mutate.c#L94))
sets `op`, `operand_base` and `is_jump_target` but **not `orig_index`** — and
`barrel_shifts`, `shift64_dead_half` and `bfi_params` are all keyed by exactly
that field. So an instruction relocated by loop rotation inherited the
destination NOP's index and silently lost its annotation: a fused
`add rd, rn, rm, lsr #2` came back as a plain `add rd, rn, rm`, dropping the
shift. This affects **any** rotated loop with an annotated op, independent of
the carried-def question, and is a bug in the shipped compiler. Fixed by
carrying `orig_index` through the rotation's save/restore (`body_origs` /
`latch_origs`).

**Step 2 (relax `ncarried_defs > 1`) — possible after that fix, but rejected
on measurement.** The guard looked like an inexplicable correctness cliff
(`>2` broke 10 ir_tests, `>3` 23, `>8` 41). It was not about carried values at
all: it was accidentally hiding the `orig_index` bug by keeping most annotated
loops from rotating. With that fixed, `> 2` passes **all 2364 ir_tests**.

It stays at 1 anyway, because rotating multi-carried loops is a measured
pessimisation: `> 2` costs **+16,418 cycles** on the `ir` suite (13,823,142 →
13,839,560) for zero byte change. Each extra carried value needs a copy at both
loop exits and another live register, which outweighs the one branch per
iteration that rotation saves. The plan assumed relaxation was a win; it is not.
The guard is now documented in-source as a performance gate with those numbers,
so nobody re-derives this.

(`> 8` additionally still fails `296_fuzz_assign_strd_deref_src` at -O2, so at
least one more latent bug sits further out.)

**Unrelated pre-existing miscompile found.** While reducing, `p4a.c` (18 lines,
an inlined hash mixer accumulating in a counted loop) diverges O0-vs-O2 — and
**still diverges with every P4 change stashed**, i.e. on committed HEAD. The ir
suite does not cover it. Saved as a starting point for its own investigation.

Steps 3–5 of §3.1 (`LOAD_POSTINC`/`STORE_POSTINC`, symbolic limits in
`decrement_to_zero`, the guard-synthesising `while` rotation) are untouched.

### §3.6 64-bit lowering — return-pair preference landed

The plan's read was right: *"prior art exists — the pr58574 soft-float work
added a return-pair r0:r1 preference; it evidently does not cover the plain
integer path."* The reason is precise. That preference
([ir/regalloc.c](../../ir/regalloc.c#L1852)) fires only when the interval is
*defined by* a call (`cur->start` is `FUNCCALLVAL`) — it keeps a call **result**
in r0:r1. The mirror case, a pair **consumed by** `RETURNVALUE`, had no hint at
all, so every 64-bit-returning function computed into a callee-saved pair and
copied out.

Added as a second trigger for the same block, with the same `!crosses_call`
guard (an intervening call would clobber r0:r1):

| | before | GCC | after |
|---|---|---|---|
| `add64` | 7 | 3 | **3** |
| `sub64` | 7 | 3 | **3** |
| `and64` / `or64` / `xor64` | 7 | 4 | **3** (beats GCC) |
| `neg64` | 9 | 3 | **5** |

`callee_long` is now three instructions with no prolog:
`adds r0,r0,r2` / `adcs r1,r3` / `bx lr` — the `push {r4,r5}` + two `mov`s +
`pop` that §3.6 identified as fixed per-function overhead is gone, and the
high-word `adcs` even came out narrow.

Measured: `.text` 127,998 → **127,974**, corpus instructions 694,566 →
**694,434 (−132)**, `ir`-suite cycles 13,823,142 → **13,822,666**. Corpus
movement is modest because 64-bit-returning functions are rare in the torture
tests, but this is fixed overhead removed everywhere it occurs.

Two expectations encoded the old allocation and were updated: the golden IR
`tests/frontend/types/27_long_long.expect` (`R4(T0)` → `R0(T0)`) and
`test_call_aapcs_long_long`, which hardcoded `adds r4, r0, r2` while its own
comment already said "result leaves in r0:r1" — it now asserts that, plus that
no `push` is needed.

Remaining §3.6 sub-levers: the spurious push/pop around 64-bit compares
(`lt64` is still 8 vs GCC 6) and inlining variable 64-bit shifts. The "narrow
encodings for 64-bit ALU halves" item is partly obtained for free — with the
result in low registers the halves now pick T16 forms.

### §3.4 epilogue-branch narrowing — landed (not in the original plan)

Return jumps pass `target_ir == -1` because the epilogue has no IR index, so
`can_narrow_forward_branch` could never size them and they were emitted wide
unconditionally — the comment at the emission site said so explicitly. But the
rehearsal pass already knows where the body ends, which is exactly where the
epilogue begins. Capturing that as `ir->codegen_rehearsal_end` lets return jumps
use the same monotonicity argument as every other forward branch, with the
pool-flush bound applied over the remaining body.

| | before | after |
|---|---|---|
| wide `b.w` in corpus | 63 (62 narrowable) | **20 (19)** |
| `.text`, 385 torture tests | 127,974 | **127,882 (−92)** |

Found by disassembling a `return K` switch probe and noticing six `b.w` to an
epilogue 78 bytes away.

### 64-bit deref LDRD/STRD pairing — landed (not in the original plan)

**Evidence.** A fresh corpus sweep found TCC emitting every 64-bit
register-deref access as two 32-bit halves: `*p` for `long long`/`double` was
`mov`+`ldr`+`ldr` (GCC: one `ldrd`), and `20021120-1::foo` — the single worst
non-`main` function at +678 — copied each of 32 doubles with 6 instructions
against GCC's 2. Statically, 698 adjacent `ldr`→`ldrd` and 223 `str`→`strd`
pairs were fusable across the 385-test corpus.

**Why it never fired.** `load_from_base`'s LDRD path is deliberately SP/FP-only
and the store path's comment says why: LDRD/STRD fault on ARMv7-M/v8-M when the
address is not 4-byte aligned (regardless of `UNALIGN_TRP`), and a general base
may point into a packed struct. The pr92904 notes called general-base pairing
"unsafe without frontend per-access alignment info plumbed through (not
available today)". This lever plumbs exactly that:

- `SValue.underaligned` (new spare bit) — set at member access when the access
  chain crosses a struct with alignment < 4 or a non-word-aligned member
  offset, and **propagated through pointer arithmetic** by a thin `gen_op`
  wrapper (`+`/`-` OR the operands' bits into the result). This is what makes
  `packed->arr[i]`, `*(packed->arr + 1)` and nested packed structs reach the
  deref still marked — verified equivalent to GCC's behavior on all five
  shapes, and stronger through casts (`(long long *)packed->member` stays
  marked; GCC assumes the cast type). `vsetc` clears the bit so reused vstack
  slots cannot leak a stale mark (staleness only costs the optimization, never
  correctness — that polarity is the design's backbone).
- `IROperand.aux` bit `IROP_AUX_ALIGN4_OK` — set only in `svalue_to_iroperand`
  for 64-bit lvalues that are NOT marked. Operands built by IR passes default
  to 0 = "not proven" = today's safe LDR/STR pair, so a pass that loses the
  bit degrades codegen, never correctness. `iroperand_to_svalue` maps it back
  conservatively so round-trips cannot re-derive a false proof.
- `MachineOperand.align4` — read at five backend sites: register-deref
  load/store, LLOCAL deref load/store, `mach_resolve_deref_64` (64-bit deref
  as ALU operand) and the 64-bit `THUMB_ARG_MOVE_MOP` deref (call arguments).

**A pre-existing packed-array fault fixed on the way.** The 64-bit
`LOAD_INDEXED`/`STORE_INDEXED` lowering has always emitted LDRD/STRD on the
computed EA — including for `packed->a[i]`, where it faults on hardware (QEMU
is lenient, so no suite caught it). The pr92904 note's claim that "indexed ops
are emitted just for naturally-aligned array accesses, never packed derefs" is
wrong for exactly this shape. Fixed with a second aux bit,
`IROP_AUX_UNDERALIGN`: the fusion passes that consume a deref operand and
install the ADD's plain base (`gens_fusion` both creators, `disp.c`,
`indexed_chain.c`) transfer the mark onto the base operand, and the indexed
emitters fall back to the LDR/STR pair. Same fix class as
`unalign-1::ptplib_send_announce`, whose `(long long *)packed_member` store
previously emitted STRD at an address ≡ 2 (mod 4) — its +1 instruction is the
only cost of the correctness repair.

**A host-compiler trap worth remembering.** The first implementation added the
two flags as individual 1-bit bitfield members next to `is_unsigned`..`is_param`.
That grew the member count of the packed 9-byte `IROperand` past what host
GCC's SRA will scalarize in by-value copies, and every whole-IR scanning pass
started round-tripping operands through byte-wise stack copies:
`tests2/101_cleanup` went **19.5s → 56.5s** to compile (98% in `ssa:vrp`'s
O(vars×instructions) prepass, per `-bench`), tripping the suite's 60s timeout.
Measured cause, not folklore: the header diff alone reproduced it. The landed
version keeps the member count unchanged by folding both flags into the
existing 4-bit pad as one `aux : 4` field with `IROP_AUX_*` masks; compile time
is back at 19.5s. Corollary: `-bench` + `PASS_TIME` is the right tool for this,
and any future IROperand flag must go into `aux`, not a new member.

**Results** (vs the epilogue-narrowing baseline, whole corpus):

| | before | after |
|---|---|---|
| corpus instructions | 694,294 | **693,062 (−1,232)** |
| ratio vs GCC | 1.0693 | **1.0674** |
| per-function | — | **330 improved / 2 regressed** |
| `.text`, 385 torture tests | 127,882 | **127,538 (−344)** |
| `ir`-suite cycles (new test excluded) | 13,822,666 | **13,822,068 (−598)** |
| `ll_load` (`return *p`) | 4 instr | **2 (== GCC)** |

Byte movement is small by design: a narrow `ldr;ldr` pair (2+2 B) fuses to a
4-byte `ldrd` — the win is instructions, cycles (3 vs 4 per pair on M33), and
freed scratch registers. Top improvements: `20021120-1::foo` −160,
`22_floating_point::main` −102, `bitfld-3::main` −54, `pr92618::foo` −34.

The two regressions: `unalign-1` +1 (the fixed latent fault, above) and
`pr50310::foo` +94 — **later shown to be a measurement artifact, not a
regression.** `foo` is 1.3 KB with mid-function literal-pool flushes;
TCC objects carry no ARM mapping symbols, so objdump loses disassembly sync
at those pools and prints real instructions as `.word` lines, which
`regression_disasm` then doesn't count.  The old build desynced harder (189
`.word` lines in `foo` against 112), so its count of 165 was a massive
*undercount*; by `readelf` symbol size the lever made `foo` 36 bytes
*smaller* (1380 → 1344).  The 6-`mov` crosswise pair swap before
`__aeabi_cdcmple` that was blamed exists identically in both builds — it is
a pre-existing regalloc argument-pair-assignment inefficiency, and corpus-wide
it is too rare to chase (one 6-mov site in 378 tests; GCC's own output has
eight ≥3-mov marshalling runs).  Metric hygiene: when a big per-function
delta shows up with a similar byte size, compare `readelf -s` sizes and
`.word`-line counts before believing it — only 2 of 2,215 corpus functions
have mid-function pools, so the corpus totals are sound, but every one of
those functions' per-function counts is untrustworthy.

Verification: `make test -j16` green at 13,670;
`diff_olevels --seeds 0-2000 --require-qemu` reports only the four
pre-existing divergences (945, 1356, 1759, 1795).

Guard tests: `tests/ir_tests/372_ldrd_align_deref.c` (runtime, QEMU: packed
direct/nested/array-const/array-var/via-arith loads and stores through a
buffer at offset 1, plus aligned-deref round-trips and the volatile-double
walk) and `test_codegen_asm.py::test_ldrd_deref_pairing_and_packed_safety`
(asserts LDRD/STRD present on aligned derefs and absent on every packed
shape). Remaining measured-but-unclaimed pairs: `20021120-1`'s spill stores at
`[sp,#>1020]` (needs the pr92904 anchor-register lever), the `20040709` family
(packed — correctly unfusable), and `20041011-1` (32-bit spill-slot pairs — a
separate RA-level lever).

### Global-base indexed fusion + rotation coverage — landed (not in the original plan)

**Found by asking why the memclr/memcpy-a* family (+11.5k gross, ~1,900
functions × +8..+11) never bottom-tests.** Two independent blockers, each
isolated by single-variable probes:

1. **A global/static array base never fused into `LOAD_INDEXED` at scale 0.**
   The unscaled path of `ir_gen_indexed_memory_fusion`
   ([gens_fusion.c](../../source/opt/flat/fusion/gens_fusion.c)) required both
   ADD operands to be vregs, so `T2 <- GlobalSym ADD V1; T3 <- *T2` stayed a
   raw ADD+DEREF — costing one instruction per access (`adds`+`ldrb` where GCC
   emits `ldrb [rb, ri]`) *and* tripping rotation's indirect-lvalue guard, which
   is why `for (i) s += gv[i]` over a global never rotated while the same loop
   over a parameter did.  The *scaled* path already admitted a SYMREF base and
   the backend materializes it (verified at -O1 where `global_addr_hoist`
   doesn't run), so the fix is symmetric admission: one operand may be a
   non-lval SYMREF (the base), with the redefinition scan skipping the
   vreg-less base.  **Corpus 693,631 → 685,376 (−8,255), ratio 1.0677 →
   1.0550, 8,232 functions improved / 3 regressed (+1 each: 20051110-1/-2).**
   The bulk is memcpy-a1/2/4/8 at −2,048 each; `ir`-suite cycles flat (the
   timed suite lacks the shape).

2. **Any call in the body rejected rotation** — including the ubiquitous
   `for (i) if (v[i] != K) abort();` check loop.  Relaxed for calls that are
   provably unreachable-from-the-loop: a **noreturn callee** anywhere in the
   body, or any call inside the cond_body cold tail (already proven
   terminating by `cold_terminates`).  The forwarding hazard the old comment
   names concerns values used after the call *returns*; a call that never
   returns cannot produce one.

   **Two profitability gates, both measured not reasoned.**  Ungating alone
   cost **+40,942** corpus instructions: every memclr/memcpy-a function
   rotated all three of its check loops and paid IV exit copies, a surviving
   pre-loop guard, and register pressure.  The landed gates require (a) the IV
   to be **dead at the loop exit** (`rot_iv_dead_after_exit` — an IV read
   after the loop, i.e. sequential check loops sharing one `i`, pays exit
   copies at both exits), and (b) the pre-loop guard to **provably fold**
   (`rot_guard_provably_folds`: IV enters from a literal on the straight-line
   path and the CMP limit is a literal, so the entry test is decidable and a
   later const fold deletes the guard; a surviving guard is a net static
   loss — memclr loop 3 measured +2/function).  With both gates the step is
   **+2 net corpus instructions** (six −1, one +8) while single check loops
   rotate: `for (i=0;i<8;i++) if (p[i]!=K) abort();` goes 13 → 10, one branch
   per iteration.  IROperand-convention trap fixed on the way: a VAR-vreg
   *lval* dest is a direct variable write (same rule as
   `ROT_LVAL_IS_INDIRECT`), so a plain-`!is_lval` def test never matches VAR
   IVs.

   The +8 is `pr22348::main`: an inlined never-taken abort check in a counted
   loop used to be const-simulated away entirely (2 instructions); the rotated
   shape no longer matches the SSA dead-loop eliminator, which runs after
   rotation.  Follow-up: teach `ssa:dead_loop` the bottom-tested shape.

**Cumulative: corpus 693,631 → 685,378 (−8,253), ratio 1.0677 → 1.0550.**
`make test` green at 13,670 (+4 from guard test), `ir`-suite cycles
13,822,178 excluding the new test (flat — the winning shapes aren't in the
timed suite).  Guard test `tests/ir_tests/373_loop_rotate_global_call.c`:
global-base sum/store loops (const and runtime limits, zero-trip, boundary),
the abort check-loop shape with the IV live across sequential loops, and a
store-diamond body.

### Loop-body inline demote — landed (regression fix, not in the original plan)

**Trigger.** A `--diff` against the committed cache showed five regressions.
One (`pr50310::foo` +94) turned out to be an objdump literal-pool desync
measurement artifact — the function actually shrank 36 bytes; see the ldrd
section above.  The other four — `builtin-bitops-1::main` +183,
`pr53163::bar` +47, `258_derived_iv_strength_reduction::main` +32,
`pr38048-2::main` +23 — turned out to be a single mechanism found by building
the committed HEAD in a worktree and diffing per function: the post-opt
inline re-decision's size gate reads `next_instruction_index` (**slots,
including residual NOPs**), and ldrd/strd pairing shrank loop-containing
callees straight through the `> 24` boundary with their real op count
unchanged (pr38048-2's `foo`: 27 slots → 24, real ops 21 both ways).  The
old threshold had been keeping loop bodies out of line *by accident* —
unfused 64-bit and indexed code was fat enough to trip it.

**Fix** ([function.c](../../source/backend/generators/function.c) re-decision
block): demote any callee whose post-opt IR still contains a backward branch
and more than 12 **real** ops (`loop_body = has_loop && real_ops > 12`).  A
body that keeps a loop is dominated by that loop at runtime, so inlining buys
only the call overhead while duplicating the loop per site — GCC keeps such
helpers out of line.  The ≤12 floor keeps tiny loop helpers inlinable for the
constant-arg path where `loop_const_sim` folds the whole thing.

**Results:** corpus 685,378 → **682,920 (−2,458), ratio 1.0550 → 1.0511**;
65 functions improved / 2 regressed.  All four reported functions restored or
better — `builtin-bitops-1::main` 2887 → **2179**, below even its
pre-regression 2709, because the demote also stopped pre-existing loop-body
inlining.  `ir`-suite cycles 13,827,486 → **13,799,699 (−27,787)**, almost
entirely `mibench_bitcount` −27,433 — real firmware code where the inlined
loop helpers were hurting.  The two accepted regressions: `pr89235::hl` +30
(second-order inline-budget redistribution on an infinite-loop OMP compile
test) and `20040409-1w::main` +19 (a constant-arg loop fold lost because the
callee now stays out of line — recovering it means keeping the eval-only
const-arg path alive through demotion, which is P6 material).

### Measurement fix: ARM mapping symbols + desync tripwire — landed

**The metric itself was broken for functions with data-in-text, and had been
mis-scoring the corpus by ~2.4k.**  Root cause chain, each stage found by
tracing actual `$d`/`$t` emissions (`TCC_MAPSYM_TRACE=1`):

1. `th_sym_d`/`th_sym_t` (thumb.c) already emitted ARM mapping symbols around
   literal pools — but **unconditionally in all three codegen passes**, so the
   discovery and rehearsal dry runs (whose offsets drift: every branch emits
   wide) planted wrong-address symbols that misled objdump *worse* than none.
   Fixed with real-pass-only gating (`map_sym_d/t` wrappers checking
   `dry_run_state.active || nocode_wanted`), and the hardcoded section index 1
   became `cur_text_section->sh_num`.
2. `erase_text_range` (tccgen.c) left symbols inside a late-reopt-erased range
   untouched, with a comment claiming stale mapping symbols are "harmless
   hints" — exactly backwards.  Stale `$d`/`$t` from replaced compile rounds
   are now retargeted to `SHN_ABS` (ignored by disassemblers; deletion would
   renumber the symtab that relocations reference by index).
3. Switch tables (`tcc_gen_machine_switch_table_mop`) emitted their in-text
   offset words with **no** mapping symbols at all.  Now wrapped in
   `map_sym_d`/`map_sym_t`.

Emitted code is byte-identical (verified by `.text` md5); only the symtab
changed.  `make test` green at 13,674.

**Corpus metric correction: TCC 682,920 → 685,370 (+2,450), honest ratio
1.0511 → 1.0548.**  72 functions changed: 37 pool-heavy ones had been
*undercounted* (post-pool code hidden as `.word`: `pr92904::main` +923,
`990326-1::main` 2→106, `pr82052::fn10` 11→119, `pr50310::foo` truly 408),
35 slightly overcounted (pool words decoding as valid instructions).  All
*deltas* measured before this fix remain valid — both endpoints shared the
same desync — but per-function claims about pool-heavy giants (e.g. the
branch-narrowing entry's `pr92904::main` −923) were partly phantom, and the
absolute ratio was flattering by ~0.4pp.

`scripts/sources/disasm_common.py` grew a tripwire so this cannot silently return:
`get_mapping_data_ranges()` reads the `$d..$t` ranges from each object and
`count_all_functions()` warns on stderr whenever decoded instructions follow
a data directive *outside* those ranges (= objdump desynced on unmarked
data).  Verified: 0 warnings corpus-wide, CSV byte-identical to the
pre-tripwire run.

**Stale-baseline caveat.**  The best-known cache had recorded impossible
bests under the broken metric (`990326-1::main` = "2 instructions" for a
288-byte function), which made every honest re-count show up as a phantom
regression.  Verified against ground truth — per-function `readelf` byte
sizes are identical between the committed compiler and this tree for all
eight flagged functions (pr50310::foo is 36 B smaller) — and both cache
files rebuilt with `--overwrite`.  `metrics/baselines/p1_baseline.csv` is left
as a historical record, but its per-function values predate the metric fix: a
`--diff p1_baseline` will show the same phantom rows for pool-heavy functions.

### §3.7 item 5 — `switch_to_data` for `RETURNVALUE` bodies — scoped, not done

The plan's measurement still holds and is worth acting on: the same six-case
switch costs **31 instructions written as `return K` and 14 written as
`r = K; break;`**, because `sd_check_case_body` only matches
`ASSIGN dst<-const; JMP merge`.

The IR shape is clean —

```
0000: JMP to 7
0001..0006: RETURNVALUE #K      <- the case bodies
0007: CMP P0,#5
0008: JMP to 10 if ">U"         <- range check -> default
0009: SWITCH_TABLE P0 #0
0010: RETURNVALUE #0            <- default
```

— and the target is `SWITCH_LOAD T <- P0` followed by `RETURNVALUE T`. The
obstacle is layout, not matching: `SWITCH_LOAD` falls through to the next
instruction, but the slot after `SWITCH_TABLE` holds the default body. Making
room means relocating the default into one of the freed case-body slots and
repointing the range-check jump at it, i.e. a full relayout of the switch
region with jump-target patching — a parallel rewrite to the existing one
(~100 lines), not an extension of it. Worth doing on a common shape; wants its
own clean run.

### §3.8 IT-block narrowing — measured and declined

The plan rates this principled and near-free (~68 B), but the corpus disagrees:
241 IT blocks contain only **48 wide instructions, 96 bytes maximum**. And the
plan's claim that `thumb.h:645`/`:655` are "the outlier" understates the work —
capturing those bytes needs `in_it_block` threaded through emitters that do not
take it (only 2 of 71 `ot_check_mov_reg` call sites pass it today, and
`th_mov_imm` has no such parameter at all). Poor return for the plumbing; not
taken.

### Cumulative position after P1 + P2 + P3

| metric | baseline `bc0e02ce` | now |
|---|---|---|
| `.text`, 385 torture tests | 131,094 | **128,004 (−3,090, −2.36%)** |
| corpus instructions | ~696,996 | **694,548 (−2,448)** |
| wide conditional branches | 1609 | 446 |
| `cbz`/`cbnz` | 0 | 127 |
| `ir`-suite cycles | 13,866,504 (381 tests) | 13,851,320 (386 tests) |
| per-function | — | **657 improved / 196 regressed** |

`make test -j16` green at 13,665. `diff_olevels --seeds 0-2000` reports only the
four pre-existing divergences (945, 1356, 1759, 1795). The 196 regressions are
+1..+9, dominated by functions that now get a dry pass and so get slightly
different register allocation; the improvements include `pr92904::main` −923 and
`118_switch::ubdg` −206.

### Verification run for all three

`make clean`-equivalent rebuild + `make test -j16` green (13,657 passed);
`scripts/diff_olevels.py --seeds 0-2000 --require-qemu` reports only the four
divergences (seeds 945, 1356, 1759, 1795) that are **pre-existing at
`bc0e02ce`**, confirmed by re-running the same seeds with every change stashed.
Do not rebuild `armv8m-tcc` while a sweep is running — it makes the sweep
report spurious `Permission denied` divergences.

### Graph-coalescer coverage: latch + loop-entry phi copies — landed

**Corpus 685,370 → 663,871 (−21,499); ratio 1.0548 → 1.0217.** 10,543
functions improved, 36 regressed (+111 total, worst +14). The single largest
lever of the whole plan, found by an idiom census over the full corpus
disassembly: TCC emitted 49,134 plain `mov rX, rY` vs GCC's 20,902, of which
20,555 were the uncoalesced increment pair `adds rX,rY,#i; mov rY,rX` (GCC
count: zero).

Root cause: `ra_coalesce_graph`'s seed-860 gate rejected every copy whose
dest is defined in more than one block unless the copy sat in a **loop
header** — but after phi resolution the `IV <- IV+1` copy sits in the
**latch**, and the cross-loop `IV2 <- IV1` handoff sits in a **split
critical-edge block**. Neither qualified, so the entire loop-IV copy class
fell back to hint-based in-scan transfer, which fails whenever the blob
interval model reports a false overlap.

Three changes ([regalloc.c](../../ir/regalloc.c),
[backedge_phi_hoist.c](../../source/opt/flat/loop/backedge_phi_hoist.c)):

1. **Latch admission** — a copy in a block with a back edge to a dominating
   header is a loop phi; admit it and let the accurate interference graph
   decide.
2. **Loop-entry admission** — a copy whose block has a unique successor that
   dominates *every other def block* of the dest (all sibling defs execute
   strictly after the copy). This admits split-critical-edge loop-entry
   copies while structurally excluding the seed-860 conditional-merge arms,
   whose sibling defs are parallel, never dominated.
3. Two latent wrong-code bugs the wider coverage exposed, both pre-existing:
   - **Deferred-PARAM liveness hole**: the coalescer's dataflow modeled a
     `FUNCPARAMVAL` source as used at the PARAM quad, but marshaling reads it
     at the CALL. A def between PARAM and CALL (the CSE'd `i+1` of a loop
     increment) looked non-interfering, coalesced, and clobbered `printf`'s
     argument (bug_stride_minimal). Fixed by mirroring `ra_build_intervals`'
     PARAM→CALL extension (cid-matched) in all three coalescer liveness
     walks.
   - **`backedge_phi_hoist` side-entry retarget**: after hoisting latch
     copies above the inverted JUMPIF, the pass retargeted other `continue`
     edges that entered the ASSIGN run straight to the loop body — skipping
     the copies. Sound only when every copy is a physical no-op; a surviving
     real `mov` left the IV un-updated on those paths (20050502-1: `i` never
     incremented on the y/z break-check paths). Now bails on side entries
     unless every ASSIGN resolved to identical registers, and bails on any
     switch-table entry into the region unconditionally.

Verification: `make clean` rebuild; `make test -j16` green (13,674);
full gcc-torture failure set **byte-identical** to unmodified HEAD (33
pre-existing, all include-path -O0 issues or known);
`diff_vs_gcc.py --seeds 860,0-249` — 0 divergences at O0/O1/O2 vs the gcc
oracle (seed 860 is the original hazard's reproducer). Compile-time canary
tests2/101_cleanup: 0.28 s → 0.31 s.

The 36 regressions are +1..+14, dominated by `backedge_phi_hoist` now
bailing where a real mov sits in a shared latch (one extra `b.n` per bail)
and by pressure shifts from longer merged intervals.

Remaining in this family: 64-bit pair-copy coalescing (~7,430 adjacent
mov-pairs, e.g. `ashrdi-1::constant_shift` computing every case into a fresh
pair) is blocked on the coalescer's function-level LLONG bail, which guards
the fragile `bug_ull_mul` UMULL half-operand spill path — fix that path
first, then relax the bail.

### Small-const mem* inline expansion (`mem_inline`) — landed, deliberately narrow

**Corpus 663,871 → 663,663 (−208); 80 fns improved (−263) / 3 regressed (+55).**
New pass [mem_inline.c](../../source/opt/flat/memory/mem_inline.c) in the
propagation group: memcpy/memset/`__aeabi_mem{cpy,set,clr}[48]` calls with a
small constant size become LOAD_INDEXED/STORE_INDEXED quads.

**The important result is the negative one.** The gap census counted 12,407
TCC mem* libcalls vs GCC's 1,887 and ranked "inline like GCC (memcpy ≤ 32,
memset ≤ 28, memmove never)" as a +10k lever.  Mirroring GCC's thresholds
measured **+46,291 corpus instructions** (1.0217 → 1.0930) and, at moderated
thresholds, **cycles −2,245 ≈ ±0.00%**: a `movs #n; bl` call site is 2-4
instructions where the expansion is 4-18, the corpus is one-shot code, and
the newlib helpers are already fast.  TCC was *winning* those functions on
the static metric by calling.  The call-count delta is a policy difference,
not a closable gap.

Landed policy = strict static-win only:
- memcpy: single piece (n ∈ {1,2,4}) → ld+st.  n==0 → call deleted.
- memset: ≤ 2 pieces (n ≤ 8), constant fill → movs + stores.
- memmove: never (matches gcc; overlap semantics).
- `-fno-builtin` / `-fno-builtin-mem{cpy,set,move}` disable it
  (NEW `NO_BUILTIN_MEMFUNCS` bit; runtime symbol-pinning tests now pass the flag).
- Expansion recycles the call's own PARAM/CALL quad slots (never inserts):
  insertion grows the NOP-inclusive slot count the auto-inline gates read and
  flipped callers' inline decisions (20141107-1 main 15→84).  Slots after the
  last foreign instruction in the param span only, so defs/aliasing stores
  keep their order.  Remaining known artifact: 20141107-1 main +69 — the
  helper now shrinks below the inline threshold and gets inlined (same gate
  quirk as the loop-body demote episode).

Three latent traps found and fixed on the way (all pre-existing):
1. **STACKOFF operands are not INDEXED bases** — the backend reads them as
   the slot's VALUE (221_fuzz: BusFault with BFAR = the copied float bits).
   Stack addresses are materialized via an explicit LEA instead.
2. **const_var_prop read-walk hole**: a store-class dest with is_lval clear
   (STORE_INDEXED base — also produced by disp_fusion) was not counted as a
   use, so a live LEA looked dead, the var's addrtaken was cleared, and a
   register-promoted local's "address" was garbage.  Fixed in the walk.
3. **LDRD/STRD pairing peepholes assume C-aligned INT32 accesses**: mem*
   pointers carry no such guarantee, and LDRD/STRD always fault unaligned.
   Expanded pieces carry IROP_AUX_UNDERALIGN on base AND dest/value operands
   (base-rewriting passes like global_base_share rebuild base operands and
   drop aux), and all five codegen pairing sites now honor the hint.

Verification: make test green (13,674); full gcc-torture failure set
byte-identical to HEAD; `diff_vs_gcc.py --seeds 0-149` 0 divergences at
O0/O1/O2; guard test `asm/mem_inline_expand.c`
(test_mem_inline_expansion_policy) pins the policy and the no-LDRD/STRD
property; tests2/101_cleanup 0.31 s (unchanged).

#### Amendment: direct slot forms (the 20141107-1 regression investigation)

The first landing expanded through LEA temps + INDEXED accesses, which
sl_forward cannot see through.  When the auto-inliner (correctly — gcc makes
the same call) started inlining the now-smaller `checkf` into `main`, each
inlined site carried an opaque 6-instruction stack dance (20141107-1 main
15→84).  The expansion now resolves its bases to the canonical slot forms:

- A STACKOFF address param, or a base temp singly-defined by `LEA T <- &slot`,
  becomes a direct `LOAD/STORE` on that slot; the LEA dies, the var stops
  being address-taken and can promote.  `fbits`-style `memcpy(&u,&f,4)`
  reinterprets now collapse (pr63843::bar 25→8, pr65369::bar 71→38,
  20141107-1 checkf 33→27, main 84→66).
- **STACKOFF offsets are abstract slot IDs remapped by frame allocation —
  never do arithmetic on them and never rebuild them** (a rebuilt/adjusted
  offset silently allocates a fresh unrelated slot; caught by -O0 runs of
  20141107-1: both copy ends landed on [sp,#8]).  Direct slot access therefore
  requires a single offset-0 piece; multi-piece stack cases decline (a
  STACKOFF INDEXED base would read the slot VALUE — the 221_fuzz trap).

Remaining 20141107-1 delta (main 15→66 vs gcc 21) decomposes into: (a) the
inline flip itself, which mirrors gcc and is enabled by checkf genuinely
improving; and (b) a pre-existing bool-normalization chain the inlining
exposes 4× — `SETIF` after a bool-returning call, compare-vs-1, second SETIF,
TEST_ZERO — that cmp_expr_fold/setif_fuse do not yet collapse.  That chain is
its own lever (checkf standalone shows the same shape).

### Bool-check chain folding — landed

**Corpus 663,663 → 663,482 (−181); 16 fns improved / 0 regressed.**
The residue the mem_inline/inline investigation exposed in 20141107-1 (`bl f;
cmp #1; ite; movne/moveq; cmp #0; beq` per inlined bool-check site, 11+
instructions vs gcc's 3) is three missing folds in
[branch.c](../../source/opt/flat/scalar/branch.c), all now in `branch_gens`:

1. **`bool_call_norm`** — `c = f(...)` where `f`'s declared return type is
   `_Bool` skips the frontend's defensive `CMP T,#0; SETIF(!=)`
   renormalization (AAPCS bool returns are 0/1; gcc makes the same
   assumption).  The CMP stays for orphan-cmp elimination.
2. **`setif_xor_invert`** — `(SETIF cond) ^ 1 → SETIF !cond` and
   `^ 0 → SETIF cond` (rematerialized at the XOR position; only NOPs may
   separate them so the CMP flags are still current).  This is the
   const-folded `(c != k) ^ b` mask of an inlined bool check.
3. **`setif_branch_fuse` generalized** — now NOP-tolerant between the four
   ops, and looks through one `U <- T` ASSIGN alias between SETIF and
   TEST_ZERO — both the *live* alias (TEST reads U) and the *dead corpse*
   variant (const-prop rewired TEST back to T, the copy lingers unread) that
   the folded `^ 0` leaves behind.  Scheduling: the fusable shape often only
   FORMS in the memory group's final const cascade, so a `setif_fuse` run was
   added to late_cleanup after `redundant_assign`.

20141107-1: main 84 → **24** (gcc 21), checkf 33 → 21; every site is now
`bl f; cmp; b<cond>`.  Other wins: builtin-prefetch-4 main −72, pr103405 −12,
pr32482 −8.  Verification: make test green, gcc-torture failure set identical
to HEAD, `diff_vs_gcc.py --seeds 0-149` 0 divergences O0/O1/O2.  Guard:
asm/bool_chain_fuse.c (no ITE, single cmp per fused check).

Cumulative for the session: 1.0548 → **1.0211** (685,370 → 663,482).

### Residual-regression triage (post bool-chain landing)

Three functions remain above their pre-session counts; all triaged, none is a
correctness issue and none is worth reverting:

- **20141107-1 main 24 vs gcc 21** (was 15 by not inlining): per-site codegen
  is now byte-parity (`bl f; cmp; b<cond>`).  The last 3 instructions vs gcc:
  gcc lays the shared abort at the FUNCTION END so every check branches
  forward and becomes `cbz/cbnz`; our abort_tail_merge picks the first
  abort's position mid-function, so later checks branch BACKWARD (`bne.n`),
  which cb* cannot encode.  Lever: place merged abort tails last (or after
  the final use) so cb-fusion fires; plus one gcc trick of recycling a
  known-0/1 register for the next call's arguments.
- **pr110603 foo 16 vs gcc 15** (+2): the function carries `sub sp,#12` /
  `add sp,#12` for a frame with ZERO sp-relative accesses — the mem_inline
  expansion removed the last frame user but `stack_size = (-loc+7)&~7` is
  never recomputed after opts.  **Census: 678 corpus functions have a
  fully-dead frame ≈ 1,311 wasted instructions** — a real lever.  Fix lives
  in ir/codegen.c stack_size computation + prologue: needs the no-VLA /
  no-FP / no-STACKOFF / no-spill / no-outgoing-args / no-scratch-save proof
  and interacts with the dry-run pass (scratch discovery, branch sizing,
  push-padding SP alignment) — plan carefully.
- **pr98853-1 foo 22 vs gcc 6** (+2 from expansion, marginal): the real gap
  is structural — gcc compiles `memcpy(2+(char*)&x, 2+(char*)&y, 2)` between
  register-held scalars to `lsrs; bfi` (SRA + bitfield insert, no memory).
  Lever: subword mem_inline copies whose slots are promotable scalars →
  UBFX/BFI register surgery.  Rare idiom in corpus (pr98853 family only).

### Dead-frame elimination, part 1: dry-run-sized scratch areas — landed

**Corpus 663,482 → 662,376 (−1,106); 565 fns improved / 10 regressed (+18).
Dead-frame census 678 → 463 functions.**

The 678 fully-dead frames (`sub sp,#N` + `add sp,#N`, zero SP accesses) were
NOT stale frontend `loc` — that is always 0 by codegen time.  They were
speculative reservations:

1. **Skip-dry-run scratch safety net (fixed)**: straight-line functions with
   a stack access (stack params or nonzero frame) reserved a blanket 16-byte
   scratch save area because `get_scratch_reg_with_save()` might need a slot
   and a PUSH would move SP.  Those functions are now demoted to the normal
   dry-run passes, which discover the ACTUAL max scratch depth and size the
   area exactly — usually zero ([ir/codegen.c] `can_skip_dry_run` now turns
   off where the old code reserved).  Compile-time canary unchanged (0.30 s);
   the dry passes emit no bytes.  Two tests updated: the codegen-call unit
   test now asserts per-arg dispatch relative to the pass count, and
   test_call_aapcs_stack_arg pins the new (smaller) stack-arg offset #8.
2. **Nested-call save area over-approximation (remaining, 311 fns ≈ 580
   insns)**: `max_nested_save_regs` computes live-across-call registers from
   `live_before & live_after_call` bitmaps and over-counts (pr110603
   reserves 4 bytes never stored to).  Needs precision work in the
   interval analysis or dry-run-informed sizing — same recipe as part 1.
3. 152 no-call dead frames remain (forward-branch functions whose dry run
   conservatively detected scratch that the real pass never used) — smaller,
   investigate with part 2.

### Dead-frame elimination, part 2: dry-run-sized nested-call save areas — landed

**Corpus 662,376 → 661,931 (−445); 242 fns improved / 0 regressed.
Dead-frame census 463 → 219 (call-class 311 → 67).**

The nested-call save area (outer-call argument registers preserved across an
inner call) was sized by a static liveness proxy (`max_nested_save_regs` from
`live_before & live_after_call` blob bitmaps) that over-approximates what the
call-site emission actually stores (`call_site->registers_map & 0xF` — only
nested argument-marshaling constructions ever save).  Same recipe as part 1:

- `CodeGenDryRunState.max_nested_saves` tracks the max slots any call site
  used; the call-site save loop records it in both passes.
- Post-dry-run, the reservation shrinks to the actual maximum (usually 0);
  loc and the outgoing/nested base fields shift by the slack.  Call sites
  address these areas by SIZE ([SP + outgoing_size]), so offsets stay
  consistent.  Skipped under FP/dynamic-SP (VLA) frames.
- Skip-dry-run functions with a nested reservation are demoted to the
  dry-run passes (as in part 1).

pr110603::foo now matches its pre-mem_inline count (frame gone,
`push {r3,lr}` alignment padding auto-decided).  Verification: make test
green, torture failure set identical to HEAD, fuzz 0-149 clean at O0/O1/O2,
101_cleanup canary 0.28 s.

Remaining 219 dead frames: 152 no-call functions where the DISCOVERY dry run
pessimistically fires scratch saves the real pass never needs (the rehearsal
pass may know better — investigate `dry_insn_saves` fidelity), and 67
call-class stragglers (FP/dynamic-SP-gated or genuinely-saving dry runs).
Diminishing; fold into any future scratch-model work.

### Duplicate literal-pool loads: IR-level rebasing REFUTED — machine-level cache is the design

Census (current corpus): TCC loads the same pool word 10,310 times redundantly
vs gcc 4,690 (+5.6k); 70% of the dups are straight-line (no branch label
between load and reload).  Root causes found in `symaddr_cse`: threshold ≥3
uses, blanket lval disqualification, rewrites limited to ADD operands and
entry-block stores.

An extended pass (threshold 2, lval loads/stores → `[T,#delta]`, symref
INDEXED bases rebased, ASSIGN/CMP/PARAM operands) was built and measured:
**+8,009 corpus** despite 1,059 locally-improved functions.  Two independent
killers, both interprocedural:

1. Running pre-const-fold blinds `global_init_prop`/const cascades
   (20040629-1 main 11 → 2,214: the callee-write tracking that folds main's
   checks stops recognizing `[T,#off]` accesses).
2. Running POST-fold doesn't help either: purity inference,
   `detect_const_result`, write summaries, `tu_no_readers` and the post-opt
   auto-inline marking all pattern-match SYMREF shapes in the FINAL IR
   (they run after codegen).  Hiding accesses behind a vreg base either
   degrades their results (main 138 vs 11) or risks unsoundness (a static
   whose reads became vreg-based looks reader-free to tu_dead_statics).

There is NO safe IR insertion point without redesigning summary extraction.
**The right design is machine-level**: codegen already keeps a single-entry
`cached_global_sym/cached_global_reg` reuse cache; extend it to a small
multi-entry cache (sym+addend keyed, invalidated on register clobber and
branch targets) so repeated materializations reuse the base register.  IR
shapes — and every TU analysis — stay untouched.

Landed from this attempt (all safe, −4): pair-typed symbols (complex /
double / long long) are excluded from symaddr_cse entirely (an lval SYMREF
carries the full type into call marshaling; a hoisted-base substitution
dropped the imaginary half of a by-value complex — complex-5), and CMP
operands now count toward the ≥3-use threshold.

### Graph-coalescer coverage 2: LLONG functions, 64-bit pair classes, diamond-arm phi copies — landed

**Corpus 662,026 → 660,257 (−1,769); ratio 1.0189 → 1.0161.** 383 functions
improved (−1,856), 19 regressed (+61, worst +9). QEMU cycles at -O2:
**−161,330 (−0.19%)**, verdict 0 test regressions. All changes inside
`ra_coalesce_graph` ([regalloc.c](../../ir/regalloc.c)) plus guard test
`380_coalesce_diamond_pair.c`.

The previous entry left 64-bit pair-copy coalescing "blocked on the fragile
`bug_ull_mul` UMULL half-operand spill path". **That premise did not
reproduce**: with the whole-function LLONG bail removed, the three
bug_ull_mul ir_tests, the full IR suite, gcc-torture, and 501 fuzz seeds ×
O0/O1/O2 are all green. The historical miscompile was evidently one of the
two latent bugs found and fixed in the first coalescer session
(deferred-PARAM liveness fits bug_ull_mul_int_accum's printf + CSE'd
increment shape exactly), not the UMULL decomposition. The bail is gone;
`TCC_NO_COALESCE_LLONG=1` restores it for attribution.

Five changes, in dependency order:

1. **Whole-function LLONG bail removed.** Alone this was only −52 net with
   55 regressions — because of #2.
2. **Pair-aware pressure gate.** The peak-pressure estimate counted only
   single INT intervals, so 64-bit-heavy functions under-reported pressure
   and coalesced into spill territory (pr112581-1 +14, 12 extra sp
   accesses). Each value now carries an INT-register weight: pair types
   (LLONG / soft-double / complex-float) count 2, complex-double 4.
   Regressions 55 → 17.
3. **LLONG-LLONG pair classes admitted** in the union gate (same-`reg_type`
   classes only; a mixed edge is a truncating/widening half-copy the
   interval model cannot express). The Briggs-style neighbor test weights
   pair neighbors 2 and drops a pair class's headroom threshold to K−1.
   Downstream is already pair-clean: rep→member propagation copies r0/r1,
   and both post-RA no-op-copy checks (`backedge_phi_hoist`,
   `post_ra_forward_diamond`) compare both pair halves. Kills the latch
   pair-copy in 64-bit accumulator loops and the switch-merge `mov/mov`
   pairs (cmpdi-1 −45, pr86637-2::uu −63, arith-rand-ll −15). −213 net at
   this point.
4. **Stage-5 `pref_reg` aggregation**: the merged class inherits the
   LATEST-ending member's soft register preference (the r0-for-RETURNVALUE
   hint), since `co_member` intervals skip the in-scan hint transfer that
   used to deliver it.
5. **Diamond-arm phi-copy admission (interval containment).** The seed-860
   gate still rejected every conditional-merge arm copy. New rule: admit
   `D <- S` when **no other def of D falls inside S's live interval
   [start,end)** — then no parallel arm can clobber a value any path still
   reads. Interval ranges are back-edge extended, so linear containment is
   conservative under loops. The transitive seed-860 hazard (S already
   coalesced with a live-through value) stays refused by the class-level
   interference check at union time. Two wrong turns worth recording:
   - *Block-local-source only* (S def'd in the copy's block, dead-out)
     admitted ONE arm of `sat_add`-style diamonds and made things WORSE
     (6→8): a partial class disables in-scan hints for the whole web and
     the surviving real copy blocks `post_ra_forward_diamond`'s branch
     elimination. Partial diamond classes are worse than none.
   - *All-or-nothing sibling checks* on top of that threw away wins where
     the sibling copy could never be fused by anyone. The containment rule
     admits BOTH arms directly and needs neither.
   This rule is what took the lever from −213 to −1,769 — it fires in
   plain 32-bit functions too (950612-1::main −18, pr111422 −15,
   pr43415 −24).

Compile-time trap hit on the way: the admission checks scanned all n
instructions per candidate edge (O(edges×n) — 101_cleanup **19 s**). Fixed
with a per-value def-position list built once; the pre-existing
`all_dominated` scan now uses it too. -O2 canary 0.28 s, unchanged.
(Separately discovered, NOT from this change: `ssa:vrp` takes 18.5 s of
101_cleanup's 19.3 s -O1 compile on baseline HEAD too — 23 calls, 1
productive; same phantom-change smell as the old dce spin. Future
compile-time lever.)

Verification: `make test` 13,676 green incl. the new guard test at 4
O-levels; gcc-torture 11,282 green; `diff_vs_gcc.py --seeds 0-499,860` 0
divergences at O0/O1/O2; `compare_worktree --baseline-commit HEAD --opt o2`
verdict clean. Known accepted residuals: pr110115 (+16 static, +53 cycles)
and 328_ssa_dead_loop (+2) are auto-inline flips (post-RA cleanup shrinks
callees' NOP-inclusive slot counts below the gate — the known
inline-redecision trap); 930421-1::f +8 and pr38554 +9 are pressure-shift
spills in call-heavy LLONG functions; the rest are +1..+4 phi-placement
shifts. Cycle regressions: nestfunc-4 +4.1%, pr110115 +26.6% (the inline
flip), rest ≤2.8% on sub-1k-cycle tests.

Remaining in this family: the 64-bit switch-merge giants
(ashrdi-1::constant_shift, 950612-1 got −18 but its `f`-helpers still
carry frames) are pressure-gate-bailed spill storms — the "64-bit spill
storm" lever, not a coalescing gap.

**Soft-double follow-up REFUTED (same day, both knobs measured and
reverted):** admitting `DOUBLE_SOFT` pair classes = **+220 corpus**
(cdivchkd/cdivchkld::match +11 each) — a merged class extends across
`__aeabi_d*` calls and defeats the in-scan return-pair r0:r1 preference
(the pr58574 lever), which handles soft-float chains better than a graph
class can. Even just weighting DOUBLE_SOFT *neighbors* 2 in the Briggs
test = **+264**: soft doubles cycle through r0:r1 returns rather than
staying register-resident, so pair-weighting them over-blocks INT/LLONG
merges. Both are documented at the union gate / Briggs test in
[regalloc.c](../../ir/regalloc.c). Do not re-attempt without teaching the
class mechanism the return-pair preference first.

### Compile-time: ssa:vrp quadratic setup — landed

**tests2/101_cleanup -O1: 19.3 s → 0.53 s (36x); IR suite wall-clock 50 s →
34 s.** Found while verifying the coalescer work: `TCC_PASS_TIMING=1` showed
`ssa:vrp` self-time at 18.47 s of the 18.7 s pass total — 23 calls, 1
productive (the phantom-cost smell from the dce fix, but here the cost was
setup, not iteration). `ssa_opt_vrp`'s stability precompute called
`sv_vreg_ever_written` + `ir_opt_vreg_address_taken_between` per PARAM and
`sv_var_write_count` + address-taken per VAR — each an O(n) instruction
scan, so setup was O((P+V)·n) per invocation before the domwalk even
started. Replaced with ONE fused scan ([vrp.c](../../source/opt/ssa/cfg/vrp.c))
collecting write counts and LEA address-taken flags for all slots at once;
the per-slot helpers are deleted. Semantics mirrored exactly (PARAM writes
count only non-lval dests; VAR writes include lval slot STOREs; taken = LEA
src1). Verification: corpus diff **byte-identical** per function, `make
test` 13,680 + torture 11,282 green. -O2 unaffected (vrp's cost there was
already amortized by smaller IR reaching it).

### Duplicate literal-pool loads, machine-level: symbol-scratch placement — landed

**Corpus 660,511 → 660,019 (−492); 674 functions improved, 24 regressed
(+~35, all +1..+9); QEMU cycles flat (−206), verdict clean.** This is the
machine-level design the refuted IR-rebasing attempt pointed to — but the
"multi-entry cached_global_sym cache" it prescribed turned out to already
exist: `imm_cache[16]` in arm-thumb-gen.c is per-register, keyed
(sym, addend), maintained identically in dry/real passes, with an elide +
reg-reg-copy reuse path in `load_full_const`. (The `cached_global_sym`
field the older memory referenced is vestigial: declared, snapshotted,
reset, never filled.)

The actual root cause of the 11.1k duplicate same-literal `ldr [pc]` loads
(GCC: 5.0k): **placement, not caching.** Symbol bases are materialized into
scratch registers, the scratch picker returns the LOWEST free register —
r0 — and r0 is also every other user's first choice (spill reloads,
marshaling), so the cached address dies before the next access.
mibench_rijndael's unrolled `encrypt` reloaded the same table base 14×
through r0 for exactly this reason.

Fix: `get_scratch_reg_for_sym_addr(sym, imm, excl)` — used by every
symbol-base materialization site (`mach_ensure_in_reg`,
`mach_writeback_dest`, deref load/store, 64-bit store, indexed bases via
ensure) — with two rules:

1. if a free register already holds &sym+imm per imm_cache, return THAT
   register: the subsequent `load_full_const` elides to zero instructions;
2. on a miss, park the address in the HIGHEST free register of R0-R3,
   away from the lowest-first churn, so rule 1 fires next time.

Both rules read only imm_cache + liveness (the dry/real-deterministic
inputs the elide path already relies on). Everything else falls back to
`get_scratch_reg_with_save` unchanged. Guard:
`asm/sym_base_reuse.c` + `test_sym_base_reuse` (six table reads / four
global stores → exactly one pool load each).

Verification: make test 13,681 green, torture 11,282 green,
`diff_vs_gcc.py --seeds 0-299,860` 0 divergences, compare_worktree verdict
0 regressions, compile-time canary unchanged (0.52 s / 0.30 s).

**Residual (~10.9k dups, census barely moved despite the win):** the
remaining population sits in register-saturated functions (rijndael encrypt
unchanged at 107) where NO free register survives between accesses — only
r0 rotates as free, so nothing can be parked. Closing that requires the
base to own a live range the allocator plans around (a materialization
interval / remat-aware spilling), i.e. the register-pressure family of
levers, not scratch placement. The census number is therefore no longer a
placement gap: what placement could reach, it got (−492 net, 674 fns).

### 64-bit switch-merge spill storm: back-edge over-extension + coalescer switch bail — landed

**Corpus 660,019 → 659,528 (−491); 726 functions improved, 25 regressed;
QEMU cycles (this + the symbol-scratch lever together, vs committed HEAD):
−533,860 (−0.64%), verdict clean.** ashrdi-1::constant_shift **640 → 394
(−246)**, builtin-bitops-1::main −112, builtin-prefetch-4 −72,
pr86637-2::uu −70, bug_packed_sizes −53.

The "64-bit spill storm" turned out to be two independent regalloc defects
that compose, neither of them register pressure:

1. **Back-edge interval over-extension** (`RA_EXTEND_BACKEDGE`,
   [regalloc.c](../../ir/regalloc.c)). A switch whose case blocks sit
   linearly BEFORE the dispatch creates backward jump edges dispatch→case.
   The extension rule stretched every interval "live at the target" to the
   jump — including single-def TEMPs *defined at* the target, which carry
   nothing across the edge (fresh def on every arrival). Each case temp
   ballooned from a 2-instruction range to [case, dispatch]: 8 pairs
   simultaneously live, all case temps spilled to PRIVATE slots with
   adjacent `str/str; ldr/ldr` round-trips (constant_shift's 488-byte
   frame vs GCC's zero). The carve-out skips the end-extension for
   single-def TEMPs whose interval starts exactly at the target, unless
   the def also READS the vreg (RMW carry) or a phi-resolution ASSIGN
   writes it inside [target, jump) (loop-carried phi dest — the existing
   `RA_VREG_HAS_ASSIGN_IN_RANGE` test). VARs keep the conservative
   behavior (multi-def cross-block flow through non-ASSIGN defs).
2. **Graph-coalescer switch bail.** `ra_coalesce_graph` refused ANY
   function containing SWITCH_TABLE/SWITCH_LOAD ("un-enumerated edges").
   Both are in fact fully modeled: `tcc_ir_cfg_build` adds every dispatch
   edge (`targets[]` is pre-filled with the default target for gap values,
   and out-of-range never reaches the dispatch — the frontend guards it
   with an explicit JUMPIF), and SWITCH_LOAD is a table-value LOAD, not a
   jump. Only IJUMP (computed goto) still bails. With the bail gone, the
   diamond-arm containment rule admits every case copy and the case temps
   coalesce with the phi destination — each case computes straight into
   the merge pair.

Diagnosis method: TCC_LOG_LS build (`touch ir/regalloc.c tccls.c && make
cross CFLAGS+='-DTCC_LOG_LS=1'`) on an 8-case repro; the log line
`T13 [2,3] -> [2,31]` under "back-edge i=31 -> target=2" was the smoking
gun — NOT the phi-operand extension, which was the first suspect.

Verification: make test 13,681 + torture 11,282 green;
`diff_vs_gcc.py --seeds 0-499,860,102` (102 = the switch-R12-clobber
fuzz reproducer) 0 divergences; compare_worktree verdict 0 regressions;
compile-time canary unchanged. Guard: `asm/sw_pair_merge.c` +
`test_switch_pair_merge_no_spill` (≤3 sp accesses: only the dispatch
index may spill — it crosses the R12-using dispatch while every
callee-saved register is busy; the broken shape was 4 per case).
Known accepted: 234_fuzz_switch_table_r12_clobber +7 static/+14 cycles
(more values legitimately classified dispatch-crossing), pr71272::f2 +4
(allocation shift).

Residual in constant_shift (394 vs GCC 224): per-case `b.w` to a shared
epilogue vs GCC's tail-duplicated `bx lr` (the return-epilogue
tail-duplication lever), and the dispatch-index spill.

### Semi-pruned SSA + STORE-def canonicalization — landed

**234_fuzz_switch_table_r12_clobber::main 217 → 126 (2.47x → 1.43x vs
GCC); corpus cache delta −34k+ / ~12k functions improved (see pending
cache run), overall corpus ratio ~1.00x.** Three composing defects, all
rooted in how promoted-var slot STOREs were modeled:

1. **Minimal (unpruned) SSA placed phis for block-local vars**
   ([ssa.c](../../ir/ssa.c) `ssa_scan_var_global`). Phi placement
   inserted phis at every iterated dominance frontier with no liveness
   check. A var whose every read follows a covering same-block def (an
   inlined helper's scratch var per switch case — csmix's `h`) can never
   carry a value across an edge, yet got a loop-header + join phi pair.
   SSA destruction materialized those as pass-through copies in EVERY
   case (6 webs × 7 copies), and the allocator kept the whole web live
   around the loop: the 234 spill storm. Now a Briggs-style upward-
   exposure scan classifies vars; block-local vars get NO phis and are
   ALWAYS promotable — including the single-block-def-in-multi-block-CFG
   shape `ssa_var_promotable` used to reject entirely (those stayed
   memory-resident with ldr/str RMW round-trips, e.g. 234's post-loop
   csmix chain: V17/V19/V21/V23 through [sp,#52]).
   Kill rules mirror the renamer exactly: a var-slot STORE kills only at
   full width vs. every read width (narrow stores leave old bytes
   observable); STRUCT/FUNC accesses force-global.
2. **In-place STORE defs were invisible to the interval builder and
   ra_co_ops** ([regalloc.c](../../ir/regalloc.c)). SSA renaming rewrites
   a promoted var's slot store to its current temp name with lval/local
   flags CLEARED — a register write. Both def/use scans classified any
   TEMP-dest STORE as a use (address), so such temps had no def at all:
   interval [0, last-use], ballooned across the loop by the back-edge
   extension, peak pressure 14 ≥ K — the graph coalescer bailed and
   cs/u4/g9 spilled with free callee-saved registers available. Both now
   classify a full-width non-lval TEMP-dest plain STORE as a def.
3. **STORE→ASSIGN conversion for covering stores of block-local vars**
   (rename time). The in-place path also hid values from the const/copy
   propagation family (multi-def temps): 257_fuzz main 133 → 120 once
   full-width INT32 slot stores of BLOCK-LOCAL vars with a plain value
   source become fresh-name ASSIGN defs. Three hard restrictions, each
   guarding a real miscompile found while landing:
   - block-local vars only: phi placement never sees STORE defs, so a
     fresh name on one branch of a GLOBAL var is lost at the join;
   - source must be a plain vreg/immediate: `V <- StackLoc[-x] [STORE]`
     is a memory LOAD — as ASSIGN it vanishes from stack-store liveness
     and DSE drops the slot's initializing stores (269 checksum);
   - INT32 width only (pairs/floats keep the in-place path).

**Exposed pre-existing holes, fixed (2):**

(1) `cpt_propagate_src2` /
`cpt_try_fold_binop` in
[const_prop_tmp.c](../../source/opt/flat/scalar/const_prop_tmp.c)
substituted constants into a barrel-shift-annotated src2 and folded the
UN-shifted value (csmix's `h >> 2` fused into `add ..., lsr #2`; 269/281
wrong checksums once more values became constants). Both sites now check
`tcc_ir_barrel_shift_at`. A CONFIG_TCC_DEBUG tripwire in
`tcc_ir_set_src2` ([tccir.h](../../tccir.h)) aborts on any immediate
substituted into an annotated src2 — the same latent hole exists
unguarded in ~30 more flat passes; the tripwire turns silent wrong code
into loud failures.

(2) `sccp`'s stack-slot store-load forwarding
([sccp.c](../../source/opt/ssa/scalar/sccp.c)) matched deref stores by
resolved stack offset but was blind to defs of an ADDRESS-TAKEN VAR
whose slot overlaps the load (`*p = 9; V = 10; return *p` folded to 9 —
gcc-torture 20041019-1/20070212-2 at -O2). Unreachable before: `&V`
chains always bottomed out at an opaque unpromoted pointer VAR; semi-
pruned promotion of LEA-dest pointer vars made the chain resolvable.
New `sccp_var_def_clobbers_slot` (VAR def + addrtaken + slot overlap ⇒
clobber) wired into all five scan paths (block scan, two between-scans,
two loop-clobber scans). Cost: 12 corpus instructions.

Verification: IR suites 2425 green, torture compile 5879 green, torture
execute 5403 green, golden `ssa:vrp/double_constant_diamond` updated (V1→T8:
a block-local var now promotes — same shape). Diagnosis: DUMP_IR_CO +
TCC_LOG_LS interval log (`T136 [0,91]` with no def was the smoking gun),
canonicalized per-pass dump diff (temp-number-insensitive) + runtime
checkpoint bisection to pin the const_prop_tmp fold.

Residual 234 gap (126 vs 88): GCC hoists csmix's common `h<<6 + h>>2`
prefix above the dispatch and sinks the shared `eors/ror/mul` tail
(cross-case code hoisting/sinking), SROA of st7 (`st7.f0 = u5` re-stored
per iteration), tbb vs 32-bit offset dispatch table.

### Pool-constant hoisting: const classes in the addr-hoist family — landed

Corpus 591,277 -> 589,160 (**-2,117**, like-for-like); 94 functions improved,
12 regressed (+40 gross).  252_fuzz main: `0x9e3779b1` was reloaded **42x**
and `0x9e3779b9` **29x** from the pool (GCC: 3x each, parked in registers at
entry); now both are register-resident, 139 -> 69 pool loads, main -52.
Guards: `asm/const_pool_hoist.c` + `test_const_pool_hoist` (1 pool load per
variant function), runtime `422_const_pool_hoist.c`.

**Design: the "remat-aware regalloc" lever's front half already existed as
the address-hoist family; constants only needed a classifier.**  The three
variants in `source/opt/ssa/memory/global_addr_hoist.c` (entry hoist across
calls / loop-preheader hoist / straight-line CSE) now also key classes on a
plain `IMM32` (class key `sym == NULL`, value in `addend`) when the constant
has no single-instruction materialization — probed via the new pure backend
predicate `tcc_gen_machine_const_needs_pool` (mirrors `th_generic_mov_imm`:
MOVS/MOV.W/MOVW, MVN for negatives) — and |v| >= 65536 (below that, MOVW or
the op-specific imm12 rewrites reach it).  Slot whitelist is narrower than
the address one: value slots of ALU/CMP/MLA ops, STORE src1, FUNCPARAMVAL,
RETURNVALUE — never LOAD/LEA/indexed operands or STORE dest (those immediate
shapes are addresses/offsets).  The hoisted def is a single-def
`ASSIGN Tn, #imm` INT32 TEMP, which is exactly `ra_mark_rematerializable`'s
eligible shape, so a spilled hoist degrades to per-use materialization (plus
the kept def) instead of stack round-trips — the back half of "remat-aware"
was already in the allocator.

Placement is what makes it safe from re-folding: the passes run in
ir/regalloc.c AFTER phi resolution and every cprop/fold (the 5157-5176
window), where nothing substitutes constants back into operands.  The
machine level cannot fix this class at all: `imm_cache` is fully reset at
every jump target and every call (ir/codegen.c), which is why the
park-high-literals and keep-imm-cache-across-IR-boundaries variants both
measured as refuted earlier.

**Const classes need their own cost model — three calibrations, each worth
its own lesson:**
1. *Reload estimate = nuses, not call-crossings* (GAH).  The call-crossing
   estimate models the symbol-scratch cache, which survives straight-line
   stretches; literals get no parking and also die at jump targets, so each
   use pays (measured: 42 uses = 42 loads).  With the address model,
   0x9e3779b1 scored 9 < the large-fn 2nd-slot bar of 12 and was skipped.
2. *Pressure-scaled bar* (GAH).  In a function with no free-window headroom
   (`tcc_ir_estimate_hoist_budget <= GAH_CONST_TIGHT_BUDGET`, i.e. 2) a
   const class must reload >= GAH_CONST_TIGHT_RELOADS (16): 389_fuzz's
   240-instr main lost 19 instructions parking two ~6-use constants in
   r8/r9 (frame 56 -> 80), while 252's 42/29-use classes clear the bar and
   win 52.  Budget 3 was measured 33 worse corpus-wide than 2.
3. *Consts are passengers, never veto addresses* (LAH).  Admitting a const
   class into a loop's all-or-nothing set canceled established address
   hoists (loop-2b::f: the INT_MAX compare vetoed the &a hoists, +4); once
   prioritized in, the same constant consumed the last budget register
   (+7: push/pop + spill slots to save one in-loop reload, plus a duplicate
   entry load because the pre-loop guard use is not rewritten).  Rule:
   consts ride only when `nclasses <= max_hoists - 2` (a register of slack),
   addresses retry alone otherwise.
4. *Straight-line break-even is 3 uses, not 2* (LAC).  In a call/branch-free
   region the imm_cache often already reuses the loaded register; a 2-use
   hoist can cost a callee-saved push/pop for nothing (andok::foo 6 -> 8).
   At 3+ uses the scratch-churn misses dominate and the temp wins.

Kill switch for bisection/A-B: `TCC_DISABLE_PASS=ssa:const_pool_hoist`
disables only the const classifier in all three variants (the address
halves keep their own pass names).

Accepted residuals: 197_fuzz_lea_fold_stack_alias::main +18 (budget >= 4 so
never "tight", but parking two 12+-use constants in r8/r9 reshuffles the
whole allocation; present in every configuration tried), scal-to-vec1::main
+6, and ~10 diffuse +1/+2.  Not attempted: I64 pool pairs (LDRD-class
constants), F32/F64, and sub-word-btype immediate operands (excluded by the
INT32-btype gate).
