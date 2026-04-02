# Phase 3 Detailed Plan: Micro-Optimize `thop_emit` and Benchmark

## Document Status
This is the actionable implementation plan for **Phase 3** of `PLAN_thop_api_inlining.md`. It depends on **Phase 1** (tables in headers + fast paths) and **Phase 2** (function-pointer elimination) being complete.

**Note:** Hand-written fast paths for hot wrappers were moved to **Phase 1b** to deliver value early. Phase 3 focuses on remaining micro-optimizations and benchmarking.

---

## 1. Objective

After Phases 1 and 2, the compiler can inline `th_add_imm` → `thop_emit` chain and the fast paths handle ~80% of common-case invocations. Phase 3 extracts the last performance wins by:

1. **Marking cold paths** so they don't pollute instruction cache.
2. **Passing `thop_args` by pointer** instead of by value to reduce register pressure.
3. **Threading `target_feat`** through `thop_emit` — only if benchmarking shows redundant global loads.
4. **Measuring** the actual compile-time speedup and iterating based on profiling data.

### Success Criteria
- Compile-time benchmark shows ≥10% speedup vs. pre-Phase-1 baseline on a large C file (e.g., `bench_algorithm.c` or `tccpp.c` self-compile).
- `thop_emit_error` is out-of-line (cold + noinline).
- `thop_args` is passed by `const thop_args *` — no stack spills for the 48-byte struct.
- Zero functional regressions (`make test -j16`).

### Rollback Criteria
- If total speedup after all 3 phases is <5%: the bottleneck is elsewhere (parser/preprocessor/IR opts). Stop optimizing codegen dispatch and profile to find the real hotspot.

---

## 2. State After Phase 2

The call chain is now fully direct and inlineable:

```
arm-thumb-gen.c (codegen)
  └── emit_alu_imm_for_op(op=ADD, ...)      [static, Phase 2]
        └── th_add_imm(rd, rn, imm, ...)    [inline, Phase 1]
              ├── FAST PATH: T16 imm8        [Phase 1b, ~80% hit]
              └── FALLBACK: thop_emit(...)   [inline, Phase 1]
                    ├── variant 0: T16 imm8  [unrolled, shape const]
                    ├── variant 1: T16 imm3  [unrolled, shape const]
                    ├── variant 2: T32 mod_imm [unrolled, shape const]
                    └── ...
```

**What the compiler still sees (in the fallback path):**
- `arm_target_dependent.feat` loaded from a global at the top of every inlined `thop_emit`
- A `for` loop (unrolled) with 2–6 variants
- `thop_emit_error` as a real function call at the bottom of every unrolled loop
- `thop_args` passed by value (48 bytes, spills to stack)

**What Phase 3 fixes:**
- Marks error paths as cold so they don't bloat the icache
- Passes `thop_args` by pointer to avoid stack copies
- Optionally hoists the feature-mask load (benchmark-driven)

---

## 3. Step-by-Step Implementation

### 3.1. Mark `thop_emit_error` as Cold and No-Inline

**Current:** `thop_emit_error` is a normal external function with no attributes. The compiler does not know it is never taken in normal compilation.

**Fix in `thumb.h`:**
```c
thumb_opcode thop_emit_error(const thop_variant *table, size_t n, thop_args a)
#ifdef __GNUC__
  __attribute__((cold, noinline))
#endif
;
```

**Fix in `thumb.c`:**
Add the same attribute to the definition.

**Effect:** The error path is placed in a cold section of the binary. The fast path no longer has a call instruction in the middle of the hot code. The compiler can reorder basic blocks to put the hot return path first.

### 3.2. Pass `thop_args` by Pointer

**Current layout** (48 bytes, exceeds register-passing capacity on x86_64 and ARM64):
```c
struct thop_args {
  uint32_t rd, rn, rm, ra;       // 16
  uint32_t imm;                  // 4
  uint32_t imm2;                 // 4
  thumb_shift shift;             // 12 (enum + uint32_t + enum)
  thumb_flags_behaviour flags;   // 4
  thumb_enforce_encoding enc;    // 4
  bool in_it_block;              // 1
  uint8_t puw;                   // 1
  // + padding → 48 bytes
};
```

**Problem:** 48 bytes passed by value spills to the stack. When `thop_emit` is inlined, the caller builds the struct on the stack, then the inlined body reads fields from it — the compiler may fail to promote all fields to registers.

**Fix:** Change `thop_emit` signature to take `const thop_args *`:

```c
static inline __attribute__((always_inline))
thumb_opcode thop_emit(const thop_variant *table, size_t n, const thop_args *a)
```

**Why this is safe and simple:**
- Since `thop_emit` is inlined, the pointer refers to a local aggregate on the caller's stack. The compiler's alias analysis handles this trivially — the local can't alias anything else.
- Zero changes at construction sites: callers already build `(thop_args){...}` as a compound literal; just take its address: `&(thop_args){...}`.
- Internal references change from `a.rd` to `a->rd` — mechanical find-and-replace inside `thop_emit` and `thop_emit_error`.

**Alternative considered and rejected:** Packing the struct (flattening `thumb_shift` fields into `thop_args`, reducing to ~36 bytes). This touches every `thop_args` initialization site across all 28 `.h` files plus `arm-thumb-gen.c`. The pointer approach is 1 signature change + mechanical `a.` → `a->` replacement inside `thop_emit`.

### 3.3. Thread `target_feat` Through `thop_emit` (Benchmark-Gated)

**Current:** `thop_emit` loads `arm_target_dependent.feat` inside the function body.

**Hypothesis:** When `thop_emit` is inlined 50+ times into a single codegen function, the compiler may reload the global 50 times.

**However:** Since `thop_emit` is inlined, the compiler sees `const thop_feat target_feat = arm_target_dependent.feat;` at every inline site. GCC and Clang at `-O2` typically CSE/hoist this load across inlined calls within the same function, keeping it in a register.

**Action:** Benchmark first. If `perf` shows redundant loads of `arm_target_dependent.feat`, then add `target_feat` as an explicit parameter:

```c
static inline __attribute__((always_inline))
thumb_opcode thop_emit(const thop_variant *table, size_t n, const thop_args *a, thop_feat target_feat)
```

And have each wrapper load it once:
```c
THOP_FAST_INLINE thumb_opcode th_add_imm(...)
{
  /* fast path ... */
  return thop_emit(..., arm_target_dependent.feat);
}
```

**If benchmark shows no redundant loads:** Skip this change entirely. Don't add churn (every wrapper's signature changes + every call site) for a benefit the compiler already provides.

### 3.4. Optimize `thop_reg_ok` for Known-Zero Constraints

After Phase 1, `s->rd_con` is a compile-time constant. When `con == 0`, the compiler already eliminates the entire call. Add a fast path for the common no-constraint case:

```c
static inline __attribute__((always_inline)) bool thop_reg_ok(uint32_t reg, reg_mask con)
{
  if (__builtin_expect(con == 0, 1))
    return true;
  if ((con & REG_LOW_ONLY) && reg > 7) return false;
  ...
}
```

This helps T32 variants that have no register constraints.

### 3.5. `__builtin_expect` — Use Sparingly

When `thop_emit` is inlined with constant shape data, many branches are resolved at compile time. `__builtin_expect` only helps for truly dynamic branches.

**Where it helps:**
- `thop_reg_ok(reg, con)` where `con` is nonzero but `reg` is usually in range (i.e., `reg <= 7` for low regs)
- `a->enc == ENFORCE_ENCODING_*` checks (most codegen uses `ENFORCE_ENCODING_NONE`)

**Where it doesn't help:**
- `thop_feat_subset(s->feat, target_feat)` where `s->feat` is a compile-time constant — the compiler already knows the branch direction
- Any branch where both operands are compile-time constants

**Action:** Add `__builtin_expect` only to the 2-3 dynamic branches identified above. Don't pepper it throughout `thop_emit`.

---

## 4. Benchmarking Plan

### 4.1. Create a Compile-Time Benchmark

Use `hyperfine` (preferred) or a shell loop for reliable sub-millisecond measurement. Do NOT use `date +%s%N` (has millisecond granularity on some systems).

**Script:** `scripts/bench_compile_time.sh`

```bash
#!/bin/bash
set -e
FILE="${1:-tests/benchmarks/bench_algorithm.c}"

# Warmup + benchmark
hyperfine --warmup 5 --runs 30 \
  "./armv8m-tcc -c $FILE -o /tmp/bench.o" \
  --export-json /tmp/bench_results.json
```

**Fallback without hyperfine:**
```bash
#!/bin/bash
set -e
FILE="${1:-tests/benchmarks/bench_algorithm.c}"
RUNS="${2:-30}"

./armv8m-tcc -c "$FILE" -o /tmp/bench.o 2>/dev/null  # warmup

total=0
for i in $(seq 1 $RUNS); do
    start=$(date +%s%N)
    ./armv8m-tcc -c "$FILE" -o /tmp/bench.o 2>/dev/null
    end=$(date +%s%N)
    total=$((total + end - start))
done

avg=$((total / RUNS))
echo "Average compile time over $RUNS runs: ${avg} ns"
```

### 4.2. Use `perf` for Micro-Analysis

```bash
perf record -g ./armv8m-tcc -c tests/benchmarks/bench_algorithm.c -o /tmp/bench.o
perf report --sort=symbol
```

Before Phase 3, `thop_emit` and its callees may be near the top. After Phase 3, they should drop significantly. If they're already low (fast paths from Phase 1b are effective), Phase 3 micro-optimizations may not be needed.

### 4.3. Verification Steps

| Step | Command | Expected Result |
|------|---------|----------------|
| 1. Baseline (pre-Phase-1) | `hyperfine ./armv8m-tcc -c tccpp.c` | Record baseline |
| 2. After Phase 1b fast paths | Same | ≥5% reduction |
| 3. After Phase 2 | Same | Additional improvement |
| 4. After Phase 3 cold paths + pointer | Same | ≥10% total reduction vs baseline |
| 5. `perf` hot list | `perf record -g ...` | `thop_emit` drops out of top-10 symbols |
| 6. IR tests | `make test -j16` | All pass |
| 7. Assembly tests | `make test-asm -j16` | All pass |
| 8. Binary size | `size bin/armv8m-tcc` | Text segment ±5% |

---

## 5. File-by-File Changes

| File | Changes |
|------|---------|
| `arch/arm/thumb/thumb.h` | 1. Pass `thop_args` by `const thop_args *`<br>2. Mark `thop_emit_error` cold<br>3. Add `__builtin_expect` to 2-3 dynamic branches |
| `arch/arm/thumb/thumb.c` | Add `__attribute__((cold, noinline))` to `thop_emit_error` definition |
| All `thop_*.h` wrappers | Update `thop_emit` call sites: `&(thop_args){...}` instead of `(thop_args){...}` |
| `arm-thumb-gen.c` | Update any direct `thop_emit` calls (unlikely but check) |
| `scripts/bench_compile_time.sh` | **New file** — compile-time benchmark |

---

## 6. Risks & Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| **No measurable speedup** from Phase 3 micro-optimizations | Medium | High | If benchmarks show <5% improvement from Phase 3 alone, the bottleneck is elsewhere. Profile with `perf` and pivot to the actual hotspot (parser, preprocessor, IR optimization). |
| **`const thop_args *` confuses alias analysis** | Very Low | Medium | The pointer refers to a compound literal in the caller's local scope. Alias analysis handles this trivially. If an issue arises, annotate with `restrict`. |
| **`__builtin_expect` not available** | Low | Low | Guard with `#ifdef __GNUC__`. TCC itself is compiled with GCC/Clang. |
| **Cold-path misattribution** | Low | Low | If `thop_emit_error` is called in unexpected cases (e.g., a valid but unusual encoding), the cold attribute slows down the error path. This is acceptable — errors are inherently cold. |

---

## 7. Expected Outcome

### Before Phase 3 (per `th_add_imm` call, after Phases 1+2)
```asm
  # th_add_imm inlined — Phase 1b fast path
  cmp $7, %edi              # rd <= 7?
  ja .Lfallback
  cmp $256, %ecx            # imm < 256?
  jae .Lfallback
  # encode T16 immediate directly — 6 instructions
  movl $0x3000, %eax
  ...
  ret

.Lfallback:
  # thop_emit fallback — unrolled loop
  movq arm_target_dependent+8(%rip), %rdx   # load feat (global)
  # 6 variants, each: load shape fields, check constraints
  ...
  call thop_emit_error       # if all fail
```

### After Phase 3 (same call)
```asm
  # th_add_imm inlined — Phase 1b fast path (unchanged)
  cmp $7, %edi
  ja .Lfallback
  ...
  ret

.Lfallback:
  # thop_emit fallback — feat load hoisted by compiler (or explicit param)
  # thop_args passed by pointer — no stack copy
  lea -48(%rsp), %rdi       # pointer to compound literal
  # 6 variants, unrolled, cold error path in .text.cold
  ...
  # thop_emit_error in cold section — not here
```

---

## 8. Post-Phase-3 State

After all three phases, the Thumb opcode emission path is fully optimized:

```
arm-thumb-gen.c
  └── emit_alu_imm_for_op (static, Phase 2)
        └── th_add_imm (inline, Phase 1)
              ├── FAST PATH: T16 immediate encoding (~80% hit rate, Phase 1b)
              │     → 4-6 instructions, zero memory loads
              │     → debug assertions validate against generic engine
              └── FALLBACK: thop_emit (inline, Phase 1)
                    → thop_args passed by pointer (Phase 3)
                    → unrolled loop with compile-time-constant shapes
                    → cold error path out-of-line (Phase 3)
```

No further architectural changes are needed. Future performance work should be driven by profiling (e.g., optimizing the parser, register allocator, or IR passes).

---

## 9. Estimation

| Task | Estimated Effort |
|------|-----------------|
| Mark `thop_emit_error` cold + noinline | 15 min |
| Pass `thop_args` by pointer + fix all sites | 1.5 hrs |
| Add `__builtin_expect` to dynamic branches | 30 min |
| Create compile-time benchmark script | 30 min |
| Run benchmarks, profile, iterate | 2 hrs |
| Benchmark-gated: thread `target_feat` (if needed) | 1.5 hrs |
| **Total** | **~4-6 hrs** (depending on whether `target_feat` threading is needed) |

---

## 10. Related Documents

- `PLAN_thop_api_inlining.md` — Parent plan
- `PLAN_thop_api_inlining_PHASE1.md` — Phase 1 (prerequisite, includes fast paths)
- `PLAN_thop_api_inlining_PHASE2.md` — Phase 2 (prerequisite)
- `THOP_GENERIC_DESIGN.md` — Original `thop_emit` design
- `tests/benchmarks/` — Target execution benchmarks (not compile-time)
