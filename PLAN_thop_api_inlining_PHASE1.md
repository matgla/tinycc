# Phase 1 Detailed Plan: Expose Tables and Shapes in Headers + Fast Paths

## Document Status
This is the actionable implementation plan for **Phase 1** of `PLAN_thop_api_inlining.md`. It covers both **Phase 1a** (tables in headers) and **Phase 1b** (fast paths for hot wrappers). It targets the thop (Thumb Opcode) infrastructure in `arch/arm/thumb/`.

---

## 1. Objective

Move all `thop_variant_shape` definitions, `TH_TABLE` instantiations, and `th_*` wrapper functions from `.c` files into their corresponding `.h` files as `static const` / `static inline`. Then add hand-written fast paths for the hottest wrappers to bypass `thop_emit` in the common case. This gives the compiler full visibility of instruction shapes when it compiles `arm-thumb-gen.c`, enabling loop unrolling of `thop_emit` and compile-time constant-folding of shape fields.

**Important:** Use `static inline` (not `__attribute__((always_inline))`) on `th_*` wrappers. Forced inlining of every wrapper causes the full `thop_emit` loop body (~100 lines, 2-6 unrolled iterations) to be duplicated at every call site in `arm-thumb-gen.c`. With ~80 call sites, this can add ~48KB of `.text` and cause significant icache pressure on the host. Let the compiler decide which wrappers to inline based on its own heuristics. Reserve `always_inline` for the Phase 1b fast paths, which are small enough (4-6 instructions) to always be profitable.

### Success Criteria
- `arm-thumb-gen.c` can inline the entire `th_add_imm(rd, rn, imm, flags, enc)` call path without LTO.
- `thop_emit` loop is unrolled for small variant counts at `-O2` (when the compiler chooses to inline).
- All existing tests pass (`make test -j16`, `make test-asm -j16`).
- `.text` + `.rodata` growth ≤ +15%. If exceeded, investigate which wrappers the compiler inlines and consider targeted `__attribute__((noinline))` on cold ones.
- Hot wrappers (`th_add_imm`, `th_sub_imm`, `th_mov_imm`, `th_mov_reg`, `th_add_reg`) hit fast paths for >80% of common-case invocations.

### Rollback Criteria
- If `.text` grows >15%: the compiler is inlining too aggressively. Add `__attribute__((noinline))` to cold wrappers or revert to `always_inline` only on fast-path wrappers.
- If compile-time benchmark shows regression: proceed immediately to Phase 1b fast paths before continuing.

---

## 2. Current Architecture (Problem Statement)

```
arm-thumb-gen.c  ──includes──►  thop_alu_imm.h  (only declarations)
                                       │
                                       ▼
                              thop_alu_imm.c   (shapes + tables + wrappers)
```

When GCC/Clang compiles `arm-thumb-gen.c`, it sees:
```c
thumb_opcode th_add_imm(uint32_t rd, uint32_t rn, uint32_t imm,
                        thumb_flags_behaviour flags, thumb_enforce_encoding encoding);
```
It **cannot** see:
- The 6 `thop_variant` entries inside `TH_ADD_IMM1`
- The `thop_variant_shape` fields (`.rd_place`, `.imm.kind`, `.feat`, etc.)
- The wrapper body that calls `thop_emit(table->variants, table->variant_count, ...)`

Consequently the compiler emits:
1. A real function call to `th_add_imm`
2. Inside `th_add_imm` (in `libthumb.a`), a loop over variants at runtime
3. Memory loads for `s->rd_place.width`, `s->feat`, etc.

**Phase 1 fixes #1 and #2 by moving wrappers+tables to headers. The shape fields in #3 become compile-time constants because `s` points into a `static const` array visible in the same TU.**

---

## 3. What Changes (High-Level)

### 3.1. Per-File Transformation Pattern

For every `arch/arm/thumb/thop_<domain>.c` + `.h` pair:

| Item | Current Location | New Location | Storage Class |
|------|-----------------|--------------|---------------|
| `static const thop_variant_shape SHAPE_*` | `.c` | `.h` | `static const` (unchanged) |
| `TH_TABLE(TH_*_IMM, ...)` | `.c` | `.h` | `static const` (unchanged) |
| `static thumb_opcode thop_*_wrapper(...)` | `.c` | `.h` | `static inline` |
| `THOP_*_FN(th_*, TH_*)` generated wrappers | `.c` | `.h` | `static inline` |
| Custom emitters (`*_emit`) | `.c` | `.h` *(see §5)* | `static inline` |
| `.c` file itself | full impl | minimal stub | `#include "thop_*.h"` only |

### 3.2. Build System Impact

**None for Phase 1.** The existing `arch/arm/thumb/Makefile` lists `thop_*.c` in `SRCS`. After the move, each `.c` becomes a one-line stub:
```c
#include "thop_alu_imm.h"
```
The object file still builds and links, but contains only dead-stripped copies of the `static` data. This keeps the Makefile untouched and minimizes risk.

---

## 4. Step-by-Step Implementation

### Step 4.1 — Create Infrastructure Helpers in `thumb.h`

Before touching individual modules, add two macros to `thumb.h` (or a new `thop_inline.h` included by all `thop_*.h`):

```c
/* Inline wrappers — let compiler decide inlining based on its heuristics.
   Reserve THOP_FAST_INLINE for small hand-written fast paths (Phase 1b)
   where inlining is always profitable (4-6 instructions). */
#define THOP_INLINE static inline

#ifdef __GNUC__
#define THOP_FAST_INLINE static inline __attribute__((always_inline))
#else
#define THOP_FAST_INLINE static inline
#endif

/* Forward-declare tcc_error for use in inline custom emitters.
   arm-thumb-gen.c already includes tcc.h before thumb.h,
   so the real macro will override this. */
#ifndef tcc_error
extern NORETURN void _tcc_error(const char *fmt, ...);
#define tcc_error _tcc_error
#endif
```

> **Rationale:** A handful of custom emitters call `tcc_error`. We cannot `#include "tcc.h"` directly in `thumb.h` (circular/fragile), but a weak forward declaration lets inline emitters compile in `arm-thumb-gen.c` where `tcc_error` is already a macro.

### Step 4.2 — Establish a Migration Order

Migrate in this order so that the simplest cases validate the pattern first.

| Batch | Files | Rationale |
|-------|-------|-----------|
| **A** | `thop_alu_imm`, `thop_alu_reg`, `thop_cmp`, `thop_shift_imm`, `thop_adr` | Zero custom emitters. Pure shapes + macros. |
| **B** | `thop_mem_imm`, `thop_mem_reg`, `thop_mem_unpriv`, `thop_mem_exclusive`, `thop_extend`, `thop_rev`, `thop_pld`, `thop_ldrex`, `thop_ldaex`, `thop_ldrd`, `thop_ldr_literal`, `thop_mrs`, `thop_mul`, `thop_dsp`, `thop_bitfield` | Zero or one simple custom emitter each. Low risk. |
| **C** | `thop_mov`, `thop_mvn`, `thop_branch`, `thop_block`, `thop_tbb`, `thop_system` | Multiple custom emitters; some need `tcc_error` handling. |
| **D** | `thop_vfp` | Largest file (9 custom emitters). Save for last. |
| **E** | `thumb.c` | Verify nothing breaks in the core utility file. |

### Step 4.3 — Mechanical Transformation (Batch A Example)

**File: `arch/arm/thumb/thop_alu_imm.h`**

Current state (~57 lines): only function prototypes.

New state (append after prototypes):

```c
static const thop_variant_shape SHAPE_T16_IMM8 = {
    .size = THOP_VARIANT_T16,
    .rd_place = {8, 3},
    .rn_place = {8, 3},
    .rd_con = REG_LOW_ONLY | REG_EQ_RN,
    .rn_con = REG_LOW_ONLY,
    .imm = {.kind = IMM_RAW, .width = 8},
    .imm_place = {0, 8},
    .implicit_s = true,
    .feat = {.t16 = 1},
};

/* ... remaining shapes ... */

#define V_IMM8(b) {&SHAPE_T16_IMM8, (b)}
#define V_IMM3(b) {&SHAPE_T16_IMM3, (b)}
#define V_MOD_IMM(b) {&SHAPE_T32_MOD_IMM, (b)}
#define V_IMM12(b) {&SHAPE_T32_IMM12, (b)}

TH_TABLE(TH_ADD_IMM, "add",
         V_IMM8(0x3000), V_IMM3(0x1C00),
         {&SHAPE_T16_ADD_SP_IMM, 0xb000},
         {&SHAPE_T16_ADD_SP_IMM8, 0xa800},
         V_MOD_IMM(0xF1000000), V_IMM12(0xF2000000));

/* --- Phase 1b fast path --- */
THOP_FAST_INLINE thumb_opcode th_add_imm(uint32_t rd, uint32_t rn, uint32_t imm,
                                         thumb_flags_behaviour flags,
                                         thumb_enforce_encoding enc)
{
    /* Fast path: T16 ADD <Rdn>, #imm8  (rd==rn, low reg, imm < 256) */
    if (__builtin_expect(enc != ENFORCE_ENCODING_32BIT && rd == rn && rd <= 7 && imm < 256, 1)) {
        thumb_opcode r = {.size = 2, .opcode = 0x3000 | (rd << 8) | imm};
#ifdef CONFIG_TCC_DEBUG
        thumb_opcode generic = thop_emit(TH_ADD_IMM.variants, TH_ADD_IMM.variant_count,
                                         (thop_args){.rd = rd, .rn = rn, .imm = imm,
                                                     .flags = flags, .enc = enc});
        assert(r.size == generic.size && r.opcode == generic.opcode);
#endif
        return r;
    }
    /* Fast path: T16 ADD <Rd>, <Rn>, #imm3 */
    if (__builtin_expect(enc != ENFORCE_ENCODING_32BIT && rd <= 7 && rn <= 7 && imm < 8, 1)) {
        thumb_opcode r = {.size = 2, .opcode = 0x1C00 | (rd << 0) | (rn << 3) | (imm << 6)};
#ifdef CONFIG_TCC_DEBUG
        thumb_opcode generic = thop_emit(TH_ADD_IMM.variants, TH_ADD_IMM.variant_count,
                                         (thop_args){.rd = rd, .rn = rn, .imm = imm,
                                                     .flags = flags, .enc = enc});
        assert(r.size == generic.size && r.opcode == generic.opcode);
#endif
        return r;
    }
    /* Fallback: generic engine */
    return thop_emit(TH_ADD_IMM.variants, TH_ADD_IMM.variant_count,
                     (thop_args){.rd = rd, .rn = rn, .imm = imm,
                                 .flags = flags, .enc = enc});
}

/* ... repeat for th_addw, th_sub_imm, th_subw, etc. ... */
```

> **Fast path pattern:** `THOP_FAST_INLINE` (forced inline) is used for wrappers with fast paths because the fast path body is small (4-6 instructions). Wrappers WITHOUT fast paths use `THOP_INLINE` (compiler-decided inlining). The `CONFIG_TCC_DEBUG` assertions continuously verify that fast paths produce byte-identical output to the generic engine — this catches encoding divergence during development, not just during one-time hash verification.

**File: `arch/arm/thumb/thop_alu_imm.c`**

New state (replace entire file):
```c
#include "thop_alu_imm.h"
```

> **Why this works:** `TH_TABLE` already uses `static const`. Moving it to a header simply gives every including TU its own copy. The compiler in `arm-thumb-gen.c` sees `TH_ADD_IMM.variant_count` as a constant `6`, and `TH_ADD_IMM.variants` as a constant array address. When `th_add_imm` is inlined into `arm-thumb-gen.c`, `thop_emit` receives compile-time-constant `table` and `n`, which triggers loop unrolling.

### Step 4.4 — Handling Custom Emitters (Batch B/C)

Custom emitters are functions of type `thop_custom_emit`:
```c
static thumb_opcode mov_reg_t1_high_emit(uint32_t base, const thop_args *a)
```

**Rule:** Move them to the header as `THOP_INLINE` **unless** they call `tcc_error` directly.

#### 4.4.1 Emitters WITHOUT `tcc_error` (majority)

Simply move to header with `THOP_INLINE` prefix. Example from `thop_mov.h`:

```c
THOP_INLINE thumb_opcode mov_reg_t1_high_emit(uint32_t base, const thop_args *a)
{
    (void)base;
    if (a->flags != FLAGS_BEHAVIOUR_SET && a->enc != ENFORCE_ENCODING_32BIT
        && a->shift.type == THUMB_SHIFT_NONE)
    {
        const uint16_t D = (a->rd >> 3) & 1;
        THOP_TRACE("mov %s, %s\n", th_reg_name(a->rd), th_reg_name(a->rm));
        return (thumb_opcode){
            .size = 2,
            .opcode = (0x4600 | (D << 7) | ((a->rm & 0xf) << 3) | (a->rd & 0x7)),
        };
    }
    return (thumb_opcode){.size = 0, .opcode = 0};
}
```

#### 4.4.2 Emitters WITH `tcc_error` (rare — only 4 instances)

Files affected:
- `thop_mov.c` — `movt_emit`
- `thop_mem_reg.c` — one emitter
- `thop_bitfield.c` — two emitters

**Strategy A (preferred):** Refactor the emitter to return a zero opcode and let the wrapper call `tcc_error` after `thop_emit` returns `{0,0}`.

Example for `movt_emit`:
```c
// In header: remove tcc_error, return zero on bad input
THOP_INLINE thumb_opcode movt_emit(uint32_t base, const thop_args *a)
{
    if (a->rd == R_SP || a->rd == R_PC || a->imm > 0xffff)
        return (thumb_opcode){0, 0};   // fail → let wrapper handle it
    /* ... normal encoding ... */
}

// In header wrapper:
THOP_INLINE thumb_opcode th_movt(uint32_t rd, uint32_t imm16)
{
    thumb_opcode r = thop_emit(TH_MOVT.variants, TH_MOVT.variant_count,
                               (thop_args){.rd = rd, .imm = imm16});
    if (!r.size)
        tcc_error("compiler_error: 'th_movt', SP or PC can't be used as rd\n");
    return r;
}
```

**Strategy B (fallback):** Keep the emitter in `.c` as a non-inline function, and reference it from the `TH_TABLE` in the header via `extern` function prototype. This blocks full inlining for that specific table only, but all other variants in the same table still benefit from unrolling. Use Strategy B only if Strategy A breaks semantics.

### Step 4.5 — Eliminate Internal Helper Macros from `.c` Files

Many `.c` files define macros like:
```c
#define THOP_ALU_IMM_FN(fn_name, table_id) ...
#define THOP_ALU_WIDE_FN(fn_name, base32) ...
```

These exist solely to generate wrapper functions. After Phase 1:
1. Expand them manually (or keep as header macros if they aid readability).
2. If kept as macros, guard with `#ifndef THOP_NO_MACROS` or similar to avoid leaking into `arm-thumb-gen.c`'s namespace.

**Recommended:** Expand them into explicit `THOP_INLINE` functions. It is ~15 lines of boilerplate per file and makes debugging/stack traces clearer.

### Step 4.6 — Include Order in `arm-thumb-gen.c`

After Phase 1, `arm-thumb-gen.c` must see the full inline wrappers (not just prototypes). Currently it includes the `.h` files for declarations and links against `libthumb.a` for implementations.

**Verify:** Does `arm-thumb-gen.c` include each `thop_*.h` directly, or does it include them via `thumb.h`? After moving implementations to `.h`, the same include paths will now pull in shapes+tables+wrappers. No new includes should be needed, but verify that all 28 `.h` files are reachable from `arm-thumb-gen.c`'s include chain.

**If not all headers are included:** Add the missing includes to `thumb.h` or to `arm-thumb-gen.c` directly.

---

## 5. File-by-File Checklist

| # | File | Custom Emitters | `tcc_error`? | Action |
|---|------|----------------|--------------|--------|
| A1 | `thop_alu_imm` | 0 | No | Move shapes + tables + wrappers. Expand macros. **Add fast paths** for `th_add_imm`, `th_sub_imm`. |
| A2 | `thop_alu_reg` | 0 | No | Same. **Add fast paths** for `th_add_reg`, `th_sub_reg`. |
| A3 | `thop_cmp` | 1 | No | Move custom emitter to header. |
| A4 | `thop_shift_imm` | 0 | No | Move shapes + tables + wrappers. **Add fast paths** for `th_lsl_imm`, `th_lsr_imm`, `th_asr_imm`. |
| A5 | `thop_adr` | 0 | No | Move shapes + tables + wrappers. |
| A6 | `thop_shift_reg` | 0 | No | Move shapes + tables + wrappers. |
| B1 | `thop_mem_imm` | 0 | No | Move shapes + tables + wrappers. |
| B2 | `thop_mem_reg` | 1 | **Yes** | Refactor emitter to return zero; error in wrapper. |
| B3 | `thop_mem_unpriv` | 0 | No | Move shapes + tables + wrappers. |
| B4 | `thop_mem_exclusive` | 0 | No | Move shapes + tables + wrappers. |
| B5 | `thop_extend` | 0 | No | Move shapes + tables + wrappers. |
| B6 | `thop_rev` | 0 | No | Move shapes + tables + wrappers. |
| B7 | `thop_pld` | 0 | No | Move shapes + tables + wrappers. |
| B8 | `thop_ldrex` | 0 | No | Move shapes + tables + wrappers. |
| B9 | `thop_ldaex` | 0 | No | Move shapes + tables + wrappers. |
| B10 | `thop_ldrd` | 1 | No | Move custom emitter to header. |
| B11 | `thop_ldr_literal` | 1 | No | Move custom emitter to header. |
| B12 | `thop_mrs` | 1 | No | Move custom emitter to header. |
| B13 | `thop_mul` | 2 | No | Move custom emitters to header. |
| B14 | `thop_dsp` | 1 | No | Move custom emitter to header. |
| B15 | `thop_bitfield` | 2 | **Yes** | Refactor emitters; error in wrapper or Strategy B. |
| C1 | `thop_mov` | 8 | **Yes (1)** | Refactor `movt_emit`; move rest inline. **Add fast paths** for `th_mov_imm`, `th_mov_reg`. |
| C2 | `thop_mvn` | 1 | No | Move custom emitter to header. |
| C3 | `thop_branch` | 5 | No | Move custom emitters to header. |
| C4 | `thop_block` | 6 | No | Move custom emitters to header. |
| C5 | `thop_tbb` | 1 | No | Move custom emitter to header. |
| C6 | `thop_system` | 1 | No | Move custom emitter to header. |
| D1 | `thop_vfp` | 9 | No | Move custom emitters to header. |
| E1 | `thumb.c` | N/A | N/A | Verify no breakage; keep as-is (it contains `thop_emit_error` and utilities). |

---

## 6. Verification & Testing

### 6.1. Build Verification
After each batch:
```bash
make clean && make cross -j$(nproc)
```

### 6.2. Functional Test Suite
```bash
make test -j16
make test-asm -j16
make test-legacy -j16
```

### 6.3. Compile-Time Inspection (Spot Check)
Select one function (e.g., `th_add_imm`) and verify the compiler unrolls:

```bash
# Compile arm-thumb-gen.c to assembly and inspect
$(CC) -O2 -S -o /tmp/arm-thumb-gen.s arm-thumb-gen.c \
  $(DEFINES) $(CFLAGS) -I. -Iir -fverbose-asm

# Look for th_add_imm call sites — there should be no `bl th_add_imm`
grep "bl.*th_add_imm" /tmp/arm-thumb-gen.s && echo "FAIL: not inlined" || echo "OK: inlined"
```

### 6.4. Size Regression Check
```bash
# Before
size bin/armv8m-tcc > /tmp/size_before.txt

# After batch D
size bin/armv8m-tcc > /tmp/size_after.txt
diff /tmp/size_before.txt /tmp/size_after.txt
```
Acceptable: ≤ +5% text+rodata growth due to duplicated tables.

---

## 7. Risks & Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| **Code bloat from `.text` duplication** | Medium | High | With `static inline` (not `always_inline`), the compiler decides which wrappers to inline. Monitor with `size bin/armv8m-tcc`. If `.text` grows >15%, add `__attribute__((noinline))` to cold wrappers. |
| **Code bloat from `.rodata` duplication** | Low | Medium | Each table is small (~6 variants × 32 bytes = ~192 bytes). With ~28 tables, duplication across 5-10 TUs is acceptable. If bloat exceeds 10%, switch to `extern const` + single-definition for non-hot paths. |
| **Fast path encoding bug** | Medium | High | Fast paths encode raw bit patterns that must be byte-identical to `thop_emit` output. `CONFIG_TCC_DEBUG` assertions validate every fast-path result against the generic engine. All existing tests run in debug mode during CI. |
| **Custom emitter `tcc_error` refactor breaks semantics** | Low | High | Use Strategy B (keep emitter in `.c`) for any refactor that isn't obviously correct. Add targeted IR test for the affected mnemonic. |
| **Macro leakage / namespace pollution** | Low | Low | Remove `THOP_ALU_IMM_FN`-style macros after expansion; don't define new macros in headers without `#undef` guards. |
| **Build failure on non-GCC/Clang compilers** | Low | Medium | `__attribute__((always_inline))` is used only on fast-path wrappers (`THOP_FAST_INLINE`), guarded by `#ifdef __GNUC__`. MSVC is not a supported target for this fork. |
| **Losing debuggability** | Low | Low | `THOP_INLINE` is just `static inline` — debuggable by default. Fast paths include debug assertions. In debug builds, the compiler won't aggressively inline, preserving stack traces. |
| **Debug build bloat from `.c` stubs** | Low | Low | Stub `.c` files include headers with `static const` data. At `-O0` the compiler may emit unreferenced copies. Acceptable for debug builds; the data is small. |

---

## 8. Post-Phase-1 State

After Phase 1 (1a + 1b) completes:

```
arm-thumb-gen.c  ──includes──►  thop_alu_imm.h
                                       │
                                       ├─ static const SHAPE_T16_IMM8
                                       ├─ static const TH_ADD_IMM
                                       └─ THOP_FAST_INLINE th_add_imm(...)
                                              ├─ FAST PATH: T16 imm8 (4-6 insns, ~80% hit)
                                              ├─ FAST PATH: T16 imm3
                                              └─ FALLBACK: thop_emit(...)
```

The compiler compiling `arm-thumb-gen.c` now sees:
1. `th_add_imm` body → inlined (forced for fast-path wrappers, compiler-decided for others)
2. Fast path checks → 4-6 instructions, common case returns immediately
3. `TH_ADD_IMM.variant_count` → constant `6` (in fallback)
4. `TH_ADD_IMM.variants[0..5]` → constant data in `.rodata` (in fallback)
5. `thop_emit` loop → unrolled 6 times with constant-folded shape fields (in fallback)

`.c` files become one-line stubs, still compiling into `libthumb.a` but contributing negligible code. Phase 2 (function-pointer elimination in `arm-thumb-gen.c`) and Phase 3 (`thop_emit` micro-optimizations) can then build on this foundation.

---

## 9. Estimation

| Batch | Files | Estimated Effort |
|-------|-------|-----------------|
| A (Phase 1a) | 6 | 2.5 hrs (pattern validation + `thop_shift_reg`) |
| A (Phase 1b) | fast paths for A1, A2, A4 | 2 hrs (write + debug-assert for `th_add_imm`, `th_sub_imm`, `th_add_reg`, `th_sub_reg`, `th_lsl_imm`, `th_lsr_imm`, `th_asr_imm`) |
| B | 15 | 4 hrs (mechanical + simple custom emitters) |
| C (Phase 1a) | 6 | 3 hrs (complex custom emitters + `tcc_error` refactor) |
| C (Phase 1b) | fast paths for C1 | 1 hr (`th_mov_imm`, `th_mov_reg`) |
| D | 1 | 1.5 hrs (VFP has many emitters but pattern is established) |
| E | Verify + fixup | 1.5 hrs |
| Testing & debugging | — | 2.5 hrs |
| **Total** | **28 files** | **~18 hrs** |

---

## 10. Related Documents

- `PLAN_thop_api_inlining.md` — Parent plan (Phases 2 & 3)
- `THOP_GENERIC_DESIGN.md` — Original design rationale for `thop_emit`
- `arch/arm/thumb/thumb.h` — Core definitions (`thop_emit`, `thop_variant_shape`, `TH_TABLE`)
