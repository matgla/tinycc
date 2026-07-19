# Writing Unit Tests

This is the authoritative guide for writing host-native C unit tests for this
tinycc fork. It covers the harness (`ut.h`), the multi-binary build layout, the
stub/linking model, the fixture and assertion patterns, and — most importantly —
the two **workflow rules** that keep test-writing safe to run alongside other
work.

> The per-directory `tests/unit/README.md` documents the same framework from the
> build-mechanics angle. This document is the process-and-conventions reference;
> read this one first.

---

## 0. Two hard rules — read before you touch anything

These two rules are non-negotiable. Everything else in this document is
mechanics; these are policy.

### Rule 1 — Do NOT modify production source code

When you are writing unit tests, you may **only add to or edit files under
`tests/unit/`**. You must **not** touch any compiler/production source:

- ❌ No edits to `tcc.h`, `tccgen.c`, `tccpp.c`, `tccelf.c`, `libtcc.c`, `tccls.c`,
  `arm-thumb-*.c`, anything under `ir/`, `arch/`, or any other `.c`/`.h` in the
  repository root or product tree.
- ✅ You *may* add/edit test files, stub files, `test_main*.c`, and the
  `tests/unit/arm/armv8m/Makefile` — these are harness files, not product code.

**Why this is strict here specifically.** Other tasks (fuzz triage, optimizer
bug-fixing, migrations) run in parallel against the same working tree, often with
their own **uncommitted** changes in `ir/*.c`. Two consequences:

1. Editing a **shared header** like `tcc.h` forces a full product rebuild, which
   recompiles *their* uncommitted `ir/*.c` into the unit-test binaries and can
   surface latent crashes a stale binary was hiding — turning a clean test run
   into a phantom failure that looks like *your* fault. (See the harness memo and
   `docs/plan_optimizer_test_coverage.md` on stale binaries.)
2. Changing product behavior to make a test pass silently couples your test task
   to their fix task, and can mask or unmask real divergences.

A unit test's job is to **characterize the code as it is**, not to change it. If
the code is wrong, see Rule 2 — you report it, you do not fix it in a
test-writing task.

If you genuinely believe a test cannot be written without a production change
(e.g. a `static` helper is unreachable, or a symbol needs weakening), **stop and
surface it** rather than editing product source. In almost every case the
existing patterns below (include-the-`.c`, a new stub, a new isolated binary)
already solve it without touching product code.

### Rule 3 — Use timeouts and run only unit tests

When running unit tests, **always use a timeout of less than 15 seconds** and **run only the unit test target** — never `make cross` or `make test`:

```bash
timeout 15 ./build_ssaopt/run_unit_tests_ssaopt   # ✅ correct
timeout 15 make -C tests/unit/arm/armv8m run      # ✅ correct (builds and runs in one step)
make cross                                        # ❌ never during test writing
timeout 300 make test                             # ❌ never during test writing
```

**Why this matters.** The full compiler build (`make cross`) takes minutes and can
interfere with other tasks running in parallel. The full test suite (`make test`)
includes QEMU-based cross-tests that take even longer and can hang on buggy code.
Unit tests should complete in under 10 seconds — if they don't, something is wrong
(an infinite loop, a hang, a memory corruption) and the timeout protects you from
waiting indefinitely.

**What to do if a test hangs.** If `timeout 15` kills your test binary, investigate
the root cause: check for infinite loops in the pass under test, missing
initialization, or memory corruption. Do **not** increase the timeout to "fix" the
hang — that just hides the problem.

### Rule 2 — Report every suspected bug in `docs/bugs.md`

If a test reveals that production code is wrong (a miscompile, an out-of-range
encoding, a dead branch, an unhandled case), **do not fix it** and **do not
delete or weaken the test**. Instead:

1. **Pin it with a characterization test** — write the test so it asserts the
   *current, buggy* behavior, and add a comment saying so. This makes the test a
   regression lock: whoever fixes the bug later must flip the assertion, which
   makes the fix visible in review.
2. **Write it up in `docs/bugs.md`** using the format in
   [§9 Bug-reporting workflow](#9-bug-reporting-workflow).

This is the same working rule the coverage effort has followed from the start
(see `tests/unit/PASS_COVERAGE.md`): *test-writing changes no production code;
suspected bugs are pinned as documented expectations.*

---

## 1. Philosophy

The suites are **host-native**: compiled with `gcc` and run directly on the build
machine — no cross-compiler, no QEMU. This gives a fast feedback loop for data
structures, algorithms, encoders, and optimizer passes in isolation.

Target fidelity comes from the preprocessor defines in the Makefile
(`-DTCC_TARGET_ARM`, `-DTCC_TARGET_ARM_ARCHV8M`, …) so `tcc.h` parses **identically**
to the armv8m cross build.

**Core design principle — stub what you don't test.** Each binary links only the
product module(s) it actually exercises, plus a thin stub layer for their
dependencies (allocators, global state, ELF/section helpers). This keeps every
binary small and fast to link, and keeps a test focused on one module.

---

## 2. The harness API (`tests/unit/ut.h`)

A single ~200-line header, no external libraries. Everything you need:

| Macro | Purpose |
|-------|---------|
| `UT_TEST(name)` | Declares **and auto-registers** a test: expands to `static int name(void)` plus a constructor that adds it to the registry. Return `0` = pass, `-1` = fail. |
| `UT_TEST_DISABLED(name)` | Declares a test **without** registering it — it keeps compiling but never runs. Use for pinned-but-broken cases (with a `docs/bugs.md` entry). |
| `UT_ASSERT(cond)` | Fail (print `file:line`, record, `return -1`) if `cond` is false. |
| `UT_ASSERT_EQ(a, b)` | Fail unless `a == b` (both cast to `long long`); prints both values. |
| `UT_ASSERT_NE(a, b)` | Fail unless `a != b`. |
| `UT_ASSERT_STREQ(a, b)` | String equality, NULL-safe. |
| `UT_SUITE_SETUP(fn)` / `UT_SUITE_TEARDOWN(fn)` | Registers `fn` to run before the file's first test / after its last. Only for genuinely shared state (see `test_tccpp.c`). |
| `UT_COVERS(pass)` | File-scope annotation: this file covers optimization pass `"pass"`. Consumed by the pass-coverage ledger. |
| `UT_MAIN_IMPL` | Instantiates the shared counters and the test registry. **Exactly one** TU per binary uses it (the `test_main*.c` for that binary). |
| `ut_run_all(argc, argv)` | Runs every registered test, grouped into suites by file name (`test_<suite>.c`). argv entries are substring filters on suite/test names. Returns the exit code. |

There is **no suite boilerplate and no registration step**: dropping a
`UT_TEST` into a `test_*.c` file that the Makefile links is all it takes. The
suite name is derived from the file name; tests run in definition order within
a file, files in link order. Run a subset with e.g.
`./build/run_unit_tests opt_dce` or `./build/run_unit_tests test_hash_insert`.

**Semantics to remember:**

- The **first failed assert in a test aborts that test** (`return -1`), but other
  tests and suites still run — you get the full picture in one run.
- A test is counted failed if it returns non-zero **or** if any assert fired in
  it (the `UT_RUN` wrapper checks both).
- Tests must be **deterministic and self-contained**: no file I/O, no network, no
  randomness, no dependence on run order.

---

## 3. The multi-binary layout

The suites are **not** one binary. `tests/unit/arm/armv8m/Makefile` builds **ten
independent binaries**, each linking a different real module (or module set) plus
its own stub layer. This exists because many product files define symbols that
another file's stub *also* defines — linking both is a guaranteed
"multiple definition" error, so they must live in separate binaries.

| # | `make` target | Binary | Links the real… | `test_main` |
|---|---------------|--------|-----------------|-------------|
| 1 | `run` (default, main) | `run_unit_tests` | the big shared module set: `ir/*`, `arch/arm/*`, `thop_*`, `arm-thumb-asm.c`, `arm-link.c`, `tccls.c`, codegen dispatch, … | `test_main.c` |
| 2 | `run-backend` | `run_unit_tests_backend` | `arm-thumb-gen.c` + `arm-thumb-callsite.c` (real emitters, no mop stubs) | `test_main2.c` |
| 3 | `run-tccgen` | `run_unit_tests_tccgen` | `tccgen.c` (`#include`d, see §7) | `test_main3.c` |
| 4 | `run-libtcc-api` | `run_unit_tests_libtcc_api` | `libtcc.c` | `test_main4.c` |
| 5 | `run-tccopt` | `run_unit_tests_tccopt` | `source/opt/engine/fp_mat_cache.c` + `pass_registry.c` | `test_main5.c` |
| 6 | `run-tccelf` | `run_unit_tests_tccelf` | `tccelf.c` | `test_main6.c` |
| 7 | `run-tccpp` | `run_unit_tests_tccpp` | `tccpp.c` | `test_main7.c` |
| 8 | `run-tcctools` | `run_unit_tests_tcctools` | `tcctools.c` | `test_main8.c` |
| 9 | `run-tccyaff` | `run_unit_tests_tccyaff` | `tccyaff.c` + `tccelf.c` | `test_main9.c` |
| 10 | `run-tcc` | `run_unit_tests_tcc` | `tcc.c` driver helpers | `test_main10.c` |

**Which binary does my suite belong in?**

- Testing an `ir/*` module, an `arch/arm/*` module, a `thop_*` encoder, the
  register allocator, the linker (`arm-link.c`), or the codegen dispatch layer →
  **the main binary (1)**. This is where almost all new suites go.
- Testing the raw Thumb-2 emitters in `arm-thumb-gen.c` (asserting the actual
  emitted bytes through the real backend) → **backend binary (2)**.
- Testing `tccgen.c` / `libtcc.c` / `tccelf.c` / `tccpp.c` /
  `tcctools.c` / `tccyaff.c` / `tcc.c` → their **dedicated isolated binary**.

The aggregate `make run` (and top-level `make ut`) runs binaries **1, 3, 6, 7, 8,
9, 10**. The backend (2), libtcc-api (4), and tccopt (5) binaries are **opt-in**
— run them explicitly with their `run-*` target. If you add a suite to an opt-in
binary, note that in your summary so it actually gets run.

---

## 4. Anatomy of a test file

A suite is one `test_<module>.c` file. Template:

```c
/*
 *  test_<module>.c - suite for <path/to/module>.c
 *
 *  Covers:
 *    - <function>(): <what and why>
 *  HARNESS NOTES:
 *    - <any non-obvious stub / fixture decisions>
 */

#define USING_GLOBALS      /* only if the module reads the tcc_state global */
#include "tcc.h"           /* or "ir.h" / a narrower header the module needs */

#include "ut.h"

/* -------------------------------------------------------- fixtures/stubs */

/* Minimal helpers that build only the fields the module touches.
 * Local stubs for the module's link-time deps go here too (see §6). */

/* -------------------------------------------------------- tests */

UT_TEST(test_feature_basic)
{
  /* Arrange */
  /* Act    */
  /* Assert */
  UT_ASSERT_EQ(subject_under_test(42), 43);
  return 0;
}

UT_TEST(test_feature_edge_case)
{
  return 0;
}

UT_COVERS("<pass_name>");   /* optional file-scope marker: only for optimizer passes */
```

`tests/unit/arm/armv8m/test_arm_link.c` is a thorough worked example: table-driven
predicate tests, byte-level encoder assertions, per-test `TCCState`/`Section`
fixtures, an error-recording stub, and `HARNESS NOTES` explaining every stub.

---

## 5. Adding a suite to the main binary

Three files, all under `tests/unit/arm/armv8m/` (all harness files — Rule 1 is
not violated):

### Step 1 — Create `test_<module>.c`

Use the template in §4. Include only what the module needs. Prefix `USING_GLOBALS`
only if the module accesses `tcc_state` (see the gotcha in §6).

### Step 2 — Wire it into the Makefile

1. Add the test file to `UT_LOCAL_SRCS`:

   ```makefile
   UT_LOCAL_SRCS := \
       ...
       test_my_new_module.c \
       ...
   ```

2. If the module under test is a product `.c` **not already** in `UT_MODULE_SRCS`,
   add it there (path relative to `$(TOP)`):

   ```makefile
   UT_MODULE_SRCS := \
       ...
       $(TOP)/ir/my_new.c \
   ```

   If it's **header-only** (e.g. `tcc-chained-hash.h`) or already listed, skip
   this. Note the split: files in `UT_COVERAGE_ONLY_SRCS` are compiled for
   coverage bookkeeping but **not linked** (their symbols are faked by the stub
   layer). Don't move a file out of that list unless you've supplied the symbols
   its real code needs.

### Step 3 — Build and run

```bash
make -C tests/unit/arm/armv8m run          # this binary only
# or the whole aggregate:
make ut
```

> The Bash tool's working directory resets to the repo root between calls — use
> `make -C tests/unit/arm/armv8m ...`, don't `cd`.

Expected success tail:

```
== suite my_new_module ==
    ok   test_feature_basic
    ok   test_feature_edge_case

<N> tests, <M> asserts, 0 failed tests, 0 failed asserts
```

Adding a suite to one of the **isolated** binaries (§3) is the same shape, but you
edit that binary's `test_mainN.c` and its `UTn_LOCAL_SRCS` list instead.

---

## 6. Stubs and linking

Because a binary doesn't link the full compiler, unresolved symbols are expected.
Resolve them with this ladder (cheapest first):

1. **Allocator / trivial helper** (`tcc_malloc`, `tcc_error`, a `read/write*le`) →
   it's probably already in a shared stub file (`stubs.c`,
   `codegen_mop_stubs.c`, `elfsec_stubs.c`, `ra_link_stubs.c`,
   `tcc_state_stub.c`). If not, add a minimal one there — or, if it's specific to
   your suite, define it **locally in your test file** (as `test_arm_link.c` does
   for `write16le`/`add32le`/`_tcc_error_noabort`).
2. **Global variable** → `tcc_state_stub.c`, or a new small stub TU.
3. **Another testable module** → add its source to `UT_MODULE_SRCS`.
4. **Something huge** (a parser, a codegen backend) → reconsider the test
   boundary. Test through a smaller API, or use the include-the-`.c` trick (§7).

**A stub must live in a file linked into exactly the binaries that need it.**
Putting an ELF stub in a *shared* file collides with the backend binary's own
copy. Match the placement to the binary: main-only stubs go in
`codegen_mop_stubs.c`/`stubs_gen_machine_fallback.c`; backend-only in
`codegen_backend_stubs.c`; shared in `stubs.c`/`elfsec_stubs.c`/`ra_link_stubs.c`/
`tcc_state_stub.c`.

**The `USING_GLOBALS` gotcha.** With `USING_GLOBALS`, `tcc.h`'s `TCC_STATE_VAR(x)`
expands to `tcc_state->x` (without it, `s1->x`). So a bare write to
`cur_text_section` / `symtab_section` / `text_section` is really
`tcc_state->...` and **segfaults if `tcc_state` is NULL**. Clear those state-vars
*before* nulling `tcc_state` at cleanup. Also: assign through the bare macro name
(`symtab_section = &sec;`), never `s1->symtab_section` — the latter
double-expands to `s1->s1->symtab_section`.

---

## 7. Reaching file-local `static` helpers: include the `.c`

Separate compilation only reaches a module's `ST_FUNC`/extern symbols. A plain
`static` helper (lowercase `t` in `nm`) is unreachable by linking. To cover those,
the test file **`#include`s the module's `.c` directly** instead of linking it:

- `test_tccgen.c` does `#define USING_GLOBALS` + `#include "tcc.h"` +
  `#include "tccgen.c"`; the tccgen binary lists **no** separate `tccgen.o`
  (linking it too would be a multiple-definition error). gcov still attributes
  coverage to `tccgen.c`.
- `test_tccasm.c` / `test_tccdbg.c` `#include` the real `.c` but must **rename**
  its statics first (`#define foo ut_foo` before the include) because they live in
  the *multi-module* main binary where those names would clash.

Use `#undef <macro>` after the include if a macro shadows a function you want to
reach. Capturing stubs (record what the module handed down to an IR/emit helper)
are the idiom for asserting on side effects when the real sink is stubbed out.

Caveat: some functions (`gv`, `vstore`, inc/dec) *link and execute* but their real
work is exactly what the stubs no-op away, so they produce nothing meaningful to
assert — that's coverage theater. Stop at the surface where the assertion is real
(e.g. `gen_op`/`gen_opic`/`gen_cast` fold constant operands in place, so you can
push two constants and assert the folded result).

---

## 8. Fixture & assertion patterns

**Pattern A — Pure predicates (no state).** Best case. Just call with inputs:

```c
UT_TEST(test_type_is_float)
{
  UT_ASSERT(tcc_ir_type_is_float(VT_FLOAT));
  UT_ASSERT(!tcc_ir_type_is_float(VT_INT));
  return 0;
}
```

**Pattern B — Module with internal pools/arrays.** Allocate a zeroed struct and
set only the fields the module touches (see `test_ir_pool.c`,
`test_ir_vreg.c` for pool/live-interval init with the right sentinels like
`INTERVAL_NOT_STARTED`, `PREG_NONE`).

**Pattern C — Encoders: assert the actual bytes.** For anything that emits code,
read the emitted bytes back and decode them; don't trust an expected
offset/size. `o(0xf000b800)` may emit only 2 bytes; high registers force a 32-bit
encoding; `flags_safe()` picks MOVW vs MOVS; `gsym(0)` is a no-op. See
`test_arm_link.c`'s `write_thumb_instruction`/`relocate` tests and the `thop_*`
suites.

**Pattern D — Error paths via a recording stub.** Replace the abort-y error
reporter with a stub that just increments a counter and stashes the message, then
assert the counter (`test_arm_link.c`'s `_tcc_error_noabort`). This exercises the
out-of-range/error branch without `longjmp`/`exit`.

Always **free** everything a helper allocates — the suite runs clean under ASan
(on by default) and LeakSanitizer.

---

## 9. Bug-reporting workflow

When a test uncovers a real defect (Rule 2), do both of these — never a fix.

### 9a. Pin the current behavior in the test

Write the assertion against the **buggy** output and label it clearly. Example
shape (adapted from `test_arm_link.c`, which has a regression-lock referencing a
`bugs.md` entry):

```c
/* Regression lock for bugs.md "<title>": <one line on the wrong behavior>.
 * This pins the CURRENT (buggy) result; flip the assertion when it is fixed. */
UT_TEST(test_thing_currently_wrong)
{
  ...
  UT_ASSERT_EQ(subject(), WRONG_BUT_CURRENT_VALUE);
  return 0;
}
```

### 9b. Add an entry to `docs/bugs.md`

`docs/bugs.md` is the single bug ledger. Append a section in the existing style:

```markdown
## Bug: <concise title>

<Which function/file (with `file.c:line`), the exact wrong behavior, the root
cause, and the observable symptom.> 

Regression lock: `tests/unit/arm/armv8m/test_<module>.c`,
`test_thing_currently_wrong` pins the current buggy behavior — flip its
assertion once fixed. Not yet fixed.

Likely fix: <optional one-paragraph sketch.>
```

Conventions that already hold across `docs/bugs.md`:

- Heading is `## Bug: <title>` (group related manifestations of one root cause
  under sub-headings, as the linker-script lexer entries do).
- Name the exact function and `file.c:line`.
- State the **observable** symptom, not just the code smell.
- Point at the regression-lock test by file **and** test-function name.
- End with `Not yet fixed.` (or the fix status).

This keeps the bug discoverable, keeps the test honest, and hands the eventual
fixer everything they need — without your test task ever editing product code.

> Divergences found by the differential fuzzer (not by a unit test) have their own
> triage path — see `docs/debugging_fuzz_divergences.md` and
> `docs/fuzz_triage_guide.md`. Confirmed compiler bugs from either route still
> belong in `docs/bugs.md`.

---

## 10. Building, running, coverage

| Command | Effect |
|---------|--------|
| `make ut` | Build + run the aggregate suite (binaries 1,3,6,7,8,9,10). |
| `make -C tests/unit/arm/armv8m run` | Same aggregate, directly. |
| `make -C tests/unit/arm/armv8m run-backend` | Backend emitter binary (opt-in). |
| `make -C tests/unit/arm/armv8m run-tccgen` | tccgen binary. Likewise `run-libtcc-api`, `run-tccopt`, `run-tccelf`, `run-tccpp`, `run-tcctools`, `run-tccyaff`, `run-tcc`. |
| `make ut-coverage` | Clean instrumented build → run → gcov/gcovr report under `build/coverage/`. |
| `make ut-clean` | Remove all unit-test build artifacts. |
| `make check-pass-coverage` | Diff `UT_COVERS` markers against the pipeline pass list (if the ledger script is present). |

**Coverage notes.** `COVERAGE=1` instruments every TU; never mix instrumented and
plain objects (the coverage target always cleans first, and a mode stamp forces a
recompile on switch). Read **per-file** numbers, not the aggregate: `ir/core.c`
is linked only for its `irop_config[]` table (the rest is `--gc-sections`-stripped,
so it reports ~1%) and drags the top-line down.

**Always do a clean re-run before trusting a result:**

```bash
make -C tests/unit/arm/armv8m clean && make -C tests/unit/arm/armv8m run
```

A stale binary reports phantom passes and phantom failures. If you were handed a
tree with uncommitted product changes, a clean rebuild is what makes the run
trustworthy (see `docs/plan_optimizer_test_coverage.md`, "rebuild before
triaging").

**Run with a timeout.** Always wrap test runs with `timeout 15` (or less) to catch
hangs and infinite loops:

```bash
timeout 15 make -C tests/unit/arm/armv8m run
# or for a specific binary:
timeout 15 ./build_ssaopt/run_unit_tests_ssaopt
```

Unit tests should complete in under 10 seconds. If a timeout kills the binary,
investigate the hang — don't increase the timeout.

---

## 11. Gotchas (learned the hard way)

- **Never rebuild the compiler while a fuzz/sweep/reducer is running** — you'll
  corrupt its in-flight binary. Coordinate, or wait.
- **Editing `tcc.h` triggers a full product rebuild** and can surface latent
  crashes in *other people's* uncommitted `ir/*.c`. This is a big part of why
  Rule 1 exists. (Also: you shouldn't be editing `tcc.h` at all during a
  test-writing task.)
- **`cp`-restoring a test file mid-session** desyncs the editor's file-state
  tracking — re-`Read` the file before the next edit.
- **Fixtures that write `symtab_section` etc. under `USING_GLOBALS`** are writing
  through `tcc_state` — order your cleanup so you don't deref NULL (§6).
- **`sym_pop` on a `c == 0` sym** derefs `tcc_state->ir`; push scope-test syms
  with nonzero `c`.
- **`tcc_ir_vreg_live_interval` returns NULL** when the interval array is
  unallocated (hand-built test IR skips liveness) — handle that in helpers rather
  than assuming an interval exists.

---

## 12. Pre-finish checklist

- [ ] Test file is `tests/unit/arm/armv8m/test_<module>.c` and is listed in the
      right `UTn_LOCAL_SRCS` (and `UT_MODULE_SRCS` if needed).
- [ ] No test depends on state left behind by a sibling test — every `UT_TEST`
      in a linked file runs, in definition order.
- [ ] **No product source was modified** — `git status` shows changes only under
      `tests/unit/` (Rule 1).
- [ ] Every suspected bug is pinned as a characterization test **and** written up
      in `docs/bugs.md` (Rule 2).
- [ ] Tests run with `timeout 15` (or less) and complete in under 10 seconds (Rule 3).
- [ ] Only the unit test target was run — never `make cross` or `make test` during
      test writing (Rule 3).
- [ ] Tests are deterministic, self-contained, and free all allocations.
- [ ] `make -C tests/unit/arm/armv8m clean && timeout 15 ... run` passes with `0 failed`
      (and the relevant opt-in `run-*` target if you added to one).
- [ ] ASan run is clean (it's on by default).
```
