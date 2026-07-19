# Plan: SSA const-string folding — `ssa:const_string_fold`

**Status:** v1 landed · memchr ported (Step 1 done) · next = classify the +12 residual · **Branch:** `legacyOptRemoval`
Part of [`plan_legacy_flat_ir_ssa_retire.md`](plan_legacy_flat_ir_ssa_retire.md).

## Why the SSA pass exists

The flat `string_calls` pass runs **before** the SSA phase, so it can only fold a call whose
string-address argument is *already* a constant `GlobalSym+addend` at that point. For
index-computed addresses like `strlen(&a[i][j] ± k)` it sees a runtime address, irreversibly
redirects `strlen → __tcc_strlen`, and later SSA address folding is wasted.

`ssa:const_string_fold` is the SSA-phase consumer: it re-runs the fold *after*
`ssa:cprop`/`fold`/`gvn`/`reassoc` have reduced the address to a constant symref.
`resolve_str_builtin_id` maps both `"strlen"` and `"__tcc_strlen"` to `STRBI_STRLEN`, so it
also folds calls the flat pass already redirected.

## Shipped architecture

`source/opt/ssa/string/` — driver + registry in `const_string_fold.c`, one TU per builtin,
interface in `str_handlers.h`:

```c
typedef struct StrFoldHandler {
  int         builtin_id;                    /* STRBI_* this handler serves */
  const char *name;
  int       (*can_fold)(const StrFoldCtx *); /* pure predicate — NO mutation */
  int       (*fold)(StrFoldCtx *);           /* mutates IR, returns #changes */
} StrFoldHandler;
```

Dispatch is O(1) by `builtin_id` through `g_by_id[]`, built once from `g_handlers[]` — no
`strcmp` chain. `can_fold`/`fold` are split so the driver skips cheaply and the predicate
stays side-effect-free.

Registered today (14): `strlen` · `strcmp` · `strncmp` · `memcmp` · `memchr` · `strcpy` ·
`strspn` · `strcspn` · `strchr` · `index` · `strrchr` · `rindex` · `strstr` · `strpbrk`.

Driver runs from the `SSA_RUN` block after `ssa:load_cse` and before `ssa:branch`, so folded
immediates feed branch collapse and DCE in the same iteration.

**Adding a handler = 4 edits:** new TU in `source/opt/ssa/string/`, `extern` line in
`str_handlers.h`, entry in `g_handlers[]`, and 2 build sites —
`source/opt/ssa/Makefile` and `tests/unit/arm/armv8m/Makefile` (SSA string files are *not*
in the selfhost list).

**Alignment hazard (already fixed, do not regress):** `BLOCK_COPY` reads its source with
`LDM`, which faults on an unaligned base. String literals are byte-granular, so tccgen
word-aligns them (`ad.a.aligned = 3` on the `TOK_STR`/`TOK_LSTR` path) and `str_strcpy.c`
keeps a defensive `addend % 4 == 0` guard. Covered by `352_ssa_const_string_fold.c`.

---

## Next phase: retire flat `string_calls`

**Measured gap (2026-07-19, -O2, 4257-test corpus):** disabling flat `string_calls` costs
**+65 bytes across 13 functions** — the whole remaining reason the flat pass exists.

| Function | Δ | Cause |
|---|---|---|
| `gcc-execute/memchr::main_test` | +41 | no SSA `memchr` handler |
| `gcc-execute/memchr-1::test_narrow` | +12 | no SSA `memchr` handler |
| `ir/bench_strcmp::main` | **−4** | flat is *worse* here |
| `ir/{100,102,103}_pure_func_*::main` | +3 each | phase ordering, not coverage |
| 7 more | +1 each | diffuse |

`string_calls` is reached only through the pipeline table, so `TCC_DISABLE_PASS=string_calls`
is a faithful measurement (no compound/direct call sites).

### Step 1 — port `memchr` (closes +53 of +65) — **DONE 2026-07-19**

Landed as [`str_memchr.c`](../source/opt/ssa/string/str_memchr.c); flat's
`ir_opt_fold_memchr_offset` is now shared through `opt_utils.h` rather than duplicated.
Re-measured gap: **+65/13 funcs → +12/10 funcs**, both `memchr` entries gone. Object-diff
against the pre-change build with the default pipeline: **0** (20431 unchanged) — the handler
only fires where flat could not. Tests: `366_ssa_memchr_fold.c` (found / absent / match past
`n` / terminator at `n == strlen+1` / index-computed base / `n` past the NUL must not fold)
plus two UT11 guards.

Flat source: [`const_string_calls.c:396-429`](../source/opt/flat/scalar/const_string_calls.c#L396-L429).
Template: [`str_strchr.c`](../source/opt/ssa/string/str_strchr.c) — same shape (returns a
pointer, or a null/`-1` sentinel on no-match).

`can_fold` — all of:
- param2 (`n`) evaluates via `ir_opt_eval_const_u64`;
- param0 evaluates to a const string via `ir_opt_eval_const_string` **and** yields a base
  operand via `ir_opt_eval_const_string_operand`;
- param1 (needle) evaluates via `ir_opt_eval_const_u64`;
- **`n <= strlen(s) + 1`** — bail otherwise. This guard is load-bearing: `memchr` may scan
  past the NUL, and the pass only knows bytes up to it.

`fold` — `ir_opt_fold_memchr_offset(s, needle, n, &off)`, then NOP the call params and rewrite
the call in place to `ASSIGN`:
- `off < 0` (not found) → `src1 = irop_make_imm32(-1, 0, IROP_BTYPE_INT32)`, i.e. value `0`
  / NULL. Mind the signature — `irop_make_imm32(vreg, val, btype)`, so the leading `-1` is
  the "no vreg" placeholder, **not** the folded value;
- else → `src1 = symref(base.sym, base.addend + off)`, preserving `is_lval`/`is_local`/
  `is_const`/btype from the base operand.

Reuse the flat helpers as-is; do not re-implement `ir_opt_fold_memchr_offset`.

### Step 2 — classify the remainder (**measured: +12 / 10 funcs**)

| Function | Δ | Class |
|---|---|---|
| `ir/{100,102,103}_pure_func_*::main` | +3 each | phase ordering |
| `bug/bug_gnu_ternary_elvis::test7_chained` · `gcc-compile/pr100576::foo` · `gcc-execute/{memcpy-bi,strcmp::main_test,string-opt-5,strlen::main_test}` · `ir/353_ssa_symref_addend_fold::main` | +1 each | diffuse |
| `ir/bench_strcmp::main` | **−4** | flat is worse |

Decide per the state machine:
- residual 0 → Branch B, delete the flat pass;
- residual is the `+3` phase-ordering class → check whether moving the flat entry later, or
  letting `ssa:const_string_fold` run an extra round, absorbs it;
- residual needs pre-SSA IR → Branch A, keep flat and stop.

### Step 3 — the redirect table is a separate concern

Flat `string_calls` also carries a `__tcc_*` **call-redirect** table
([`:367-375`](../source/opt/flat/scalar/const_string_calls.c#L367-L375)) with no SSA analog.
The frontend already owns redirects (`strbi_is_redirect_target`, `tccgen.c`), so this looks
like a dead fallback — but **prove it fires zero times before deleting**; the +65 measurement
cannot separate it from the folds. If it is live, it stays flat (it is lowering, not
optimization) and only the fold half retires.

### Validation

`make test` (13556) + `make ut` + object-diff at -O2. New IR test under `tests/ir_tests/` for
const `memchr` — found, not-found, and `n` past the NUL (must **not** fold). UT11 negative
guards mirroring the `str_strchr` tests. Fuzz differential is user-run.
