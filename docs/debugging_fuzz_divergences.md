# Debugging a fuzz divergence (LLM playbook)

End-to-end workflow an agent (or human) should follow when a differential-fuzz
seed produces different output at two optimization levels. This complements
`docs/fuzz_triage_guide.md` (which covers the *sweep* + triage infrastructure);
this document is the **per-bug investigation → fix → regression-test** loop.

Golden rule: **tcc -O0 is the trusted oracle.** If O1/O2/Os disagree with O0, an
optimizer is broken. Ground truth is `gcc -m32 -funsigned-char` (ARM ABI:
unsigned char, 32-bit long) — never plain `gcc`.

---

## 0. Before you start

```bash
cd libs/tinycc
make cross -j$(nproc)          # armv8m-tcc must be current after every edit
```

## 1. Reproduce + confirm the divergence

```bash
# one seed, all O-levels self-consistency:
python3 scripts/diff_olevels.py --seed N --require-qemu
# ground truth (must equal tcc -O0):
bash tests/fuzz/runseed.sh tests/fuzz/fuzz_triage_repros/seedN.c -O0
```

If `diff_olevels.py` reports `DIVERGE`, note the failing level (the `high` level)
and the correct O0 checksum.

## 2. Run the automated bisector

```bash
python3 scripts/bisect_opt.py --seed N --high=-O1
# or, for an existing .c repro:
python3 scripts/bisect_opt.py --file tests/fuzz/fuzz_triage_repros/seedN.c --high=-O2
```

(Use `--high=-O1` with `=` — argparse needs it because the value starts with `-`.)

The script reports two cross-checked signals:

- **Phase A — culprit knob(s), QEMU-confirmed.** Every `-fno-<knob>` whose
  removal at the failing level restores the O0 signature. *All* such knobs are
  listed (a real root cause is often gated by more than one; e.g. seed 295 was
  fixed by `-fno-store-load-fwd`, `-fno-const-prop`, **and**
  `-fno-indexed-memory`). The most specific one is the pass that *creates* the
  bad value; the others are passes that *propagate* it.
- **Phase B — the exact IR fold.** Dumps IR after every pass and flags where a
  memory read (`LOAD` / `LOAD_INDEXED` / `***DEREF***`) at a stable instruction
  address turns into a constant `#...` — the classic misfold signature. Prints
  the before/after lines and the pass (group) name, plus, for each culprit knob,
  the individual passes it gates (the functions to open in `ir/opt_*.c`).

The intersection of "fold in group X" + "culprit knob gates pass X" is the bug
location. For seed 295 this was: fold in `entry_store_group` + `store-load-fwd`
gates `entry_store` → `ir/opt_memory.c:tcc_ir_opt_entry_store_prop`.

**Phase B only detects memory→constant folds.** For bugs that drop a store,
rewrite control flow, or mis-thread a branch (e.g. seed 671, where
jump-threading dropped `arr8[0] = arr9[u5&7]` from a loop), Phase B is silent
and the script automatically falls back to **Phase C** (below).

## 3. Read the IR + locate the code

### Phase C — final-IR diff (the general fallback)

After Phase A, `bisect_opt.py` automatically diffs the final optimized IR at
`high` vs `high -fno-<knob>` for the most specific culprit knob, and you can
re-run it on a reduced repro:

```bash
python3 scripts/bisect_opt.py --file reduced.c --high=-O2 --diff-knob jump-threading
```

Read the diff for instructions present on the **correct** (`+`) side but absent
on the buggy (`-`) side — that is the dropped computation. (Reducing first is
important: on a full 100-line seed, O2 unrolling/rotation makes the diff too
noisy; on a 56-line reduced repro the dropped `arr8[0]=arr9[3]` store stands
out immediately.) For seed 671 this diff pinpointed the missing store in one
read, naming `ir/opt_jump_thread.c` (`tcc_ir_opt_jump_threading`).

### Manual IR walk

If you prefer, dump the full pass sequence directly:

```bash
./armv8m-tcc -dump-ir-passes=all -O1 -nostdlib -mcpu=cortex-m33 -mthumb \
    -mfloat-abi=soft -ffunction-sections \
    -Itests/ir_tests/libc_includes -Itests/ir_tests/libc_imports \
    -Itests/ir_tests/libc_includes/newlib -Iinclude \
    -c repro.c -o /dev/null   # > passes.txt 2>&1
```

Match the `BEFORE`/`AFTER` lines from the bisector against `=== AFTER <pass> ===`
blocks. Grep the pass/group name in `ir/` to find the implementing function.

### When Phase A finds no knob

SSA-pipeline bugs are not gated by `-fno-*`. Follow the `TCC_SKIP_SSA` /
`TCC_SKIP_SSA2` env-var bisection in `docs/fuzz_triage_guide.md` ("When
`culprit knob = none`"). Pass names: `ssa:sccp ssa:cprop ssa:fold ssa:gvn
ssa:reassoc ssa:strength ssa:narrow ssa:dce ssa:dead_loop ...`.

### Reducing a huge repro

```bash
python3 scripts/reduce_divergence.py tests/fuzz/fuzz_triage_repros/seedN.c \
    --low -O0 --high -O2 -o reduced.c
```

Line-granularity delta reduction that preserves the divergence. Use it to shrink
a 100-line fuzz seed before reading IR.

## 4. Write the regression test FIRST

**Do not fix the bug before the test exists.** The test must fail on the unfixed
build and pass after the fix — that is the only proof the fix is real.

Pattern (see `tests/ir_tests/193_…199_`, `204_fuzz_entry_store_loop_overwrite`):

1. Copy the (ideally reduced) repro to `tests/ir_tests/NN_fuzz_<root_cause>.c`
   with a header comment naming the pass, the root cause, and the fix in one
   sentence. `NN` = next free number.
2. Create `tests/ir_tests/NN_fuzz_<root_cause>.expect` containing the single
   correct line, e.g. `checksum=47b835f7` (the `gcc -m32 -funsigned-char` value).
3. Register it in `TEST_FILES` in `tests/ir_tests/test_qemu.py`.
4. Confirm it **fails** on the buggy code and **passes** after the fix:
   ```bash
   git stash push ir/<thefile>.c && make cross -j$(nproc)
   cd tests/ir_tests && python run.py -c NN_fuzz_<cause>.c --cflags="-O1"   # wrong value
   git stash pop && make cross -j$(nproc)
   python run.py -c NN_fuzz_<cause>.c --cflags="-O1"                        # correct value
   ```

## 5. Fix, then verify broadly

```bash
make cross -j$(nproc)
# the new regression test at every level:
cd tests/ir_tests && for o in -O0 -O1 -O2; do python run.py -c NN_fuzz_<cause>.c --cflags="$o"; done
# full IR suite (must stay green):
python3 -m pytest test_qemu.py -n 16 -q
# confirm no new fuzz divergences were introduced in the bug's neighbourhood:
python3 scripts/diff_olevels.py --seeds 0-5000 --require-qemu
```

A fix is only complete when: the new regression test passes, the full IR suite is
green, and the fuzz sweep shows **zero new** divergences (pre-existing unrelated
ones are expected — compare against `fuzz_triage_0_5000.md`).

---

## Pitfalls & lessons

- **Reduce first, always.** Phase C (final-IR diff) and manual IR reading are
  only readable on a *reduced* repro. At -O2 a full fuzz seed unrolls/rotates
  into hundreds of lines of noise; the 56-line reduced form surfaces the single
  dropped store. Run `scripts/reduce_divergence.py` before reading IR.
- **Instrument a COPY, keep the repro pristine.** The `trace(__LINE__)` technique
  from `fuzz_triage_guide.md` is great for finding the first divergent
  statement, but the `printf` calls perturb optimization (they prevent
  unrolling/inlining), so the instrumented build can produce the *correct*
  result and mask the bug (seed 671). Always instrument a throwaway copy and
  keep the pristine repro for IR dumping.
- **One bug, many "fixing" knobs.** A misfolded constant (or dropped store)
  flows through several later passes, so disabling any of them can mask the
  symptom. The real root cause is the pass that *creates* the bad value (the one
  Phase B/C flags), not the first knob Phase A reports. Cross-check the phases.
- **Entry-block stores dominate, but domination ≠ "still current".** A store in
  the entry block executes before all code, but a later store (often inside a
  loop, reached via the back-edge) overwrites the value. Forwarding the entry
  value into a loop-interior read is wrong. This was seed 295's bug
  (`entry_store_prop`). Any "entry-BB value forwarding" pass must invalidate an
  offset the moment it is written after the entry block — and *not* be shielded
  by "but a runtime-indexed load might read it": runtime loads read memory
  directly and are unaffected by the forwarding table.
- **`-O0` is the oracle, but `char`/`long` ABI matters.** Always compare against
  `gcc -m32 -funsigned-char`; plain `gcc` (signed char) makes correct ARM code
  look wrong.
- **Size-sensitive tests.** A codegen-layout change can break tests like
  `96_nodata_wanted` (labels-as-values / literal pools). If a "fix" breaks an
  unrelated test, suspect literal-pool or branch-range regressions, not the test.
