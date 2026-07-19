# QEMU cycle profiling (mps2-an505)

Deterministic per-test cycle counts for the QEMU corpus, for A/B comparison of
optimizer changes. Measures the *generated code*, not the compiler
(`tests/ir_tests/profile_suite.py` profiles the compiler itself).

`metrics/qemu_corpus.py` compiles, links and runs each test once, yielding both
a functional verdict and a cycle count from that single pass.
`metrics/compare_worktree.py` runs it on both sides by default, so one report
carries correctness, code size and cycles (`--no-cycles` opts out of the whole
pass, gate included). The whole runnable corpus (~2300 tests) links and runs in
~11s at `-j12`, cheaper than the code-size compile it rides alongside.

That pass, not `make test`, is now the correctness gate: a test passing on the
baseline and failing on the worktree fails the exit. The verdict is deliberately
cruder than `tests/ir_tests/test_qemu.py` (no tagged/args/xfail handling) and is
only ever compared against the other side of the same diff, so tests it
mishandles fail on both sides and cancel. Absolute pass counts from it are not a
`make test` verdict.

Scope matters for signal: over the full corpus a recent branch showed
-704,331 cycles (-0.70%) with 610 tests better and 198 worse, while the `float`
suite alone showed -0.00% — its counts are dominated by newlib `printf`/softfp,
which dilutes anything tcc does. Prefer the full corpus, or a compute-heavy
suite like `ir` (which holds the mibench benchmarks).

## Recipe

Link `tests/ir_tests/qemu/mps2-an505/cyc_shim.c` alongside the test object and
run under `-icount`:

    qemu-system-arm -machine mps2-an505 -nographic -semihosting \
        -icount shift=5 -kernel test.elf

The shim starts SysTick in a constructor and prints `##CYCLES <n>` from a
destructor, so test sources are untouched and their own output is unchanged —
the marker lands after it. Parse the marker; ignore it when matching `.expect`.

## Why this mechanism

Three alternatives were measured and rejected on this host:

| Mechanism | Result |
|---|---|
| QEMU TCG plugin (`libinsn.so`) | Fedora's qemu 10.2.2 is built without the plugin interface |
| DWT_CYCCNT | Not implemented on QEMU's Cortex-M33: CTRL reads back 0, CYCCNT never advances |
| Semihosting SYS_ELAPSED | Host wall-clock (1 GHz tickfreq), non-deterministic and non-linear |
| **SysTick + `-icount`** | **Deterministic and exactly linear** |

Without `-icount`, SysTick tracks host wall time: repeated runs disagree and a
4000-iteration loop can report *fewer* ticks than a 1000-iteration one. Under
`-icount shift=N` virtual time advances 2^N ns per instruction, so SysTick
becomes a deterministic function of instructions retired.

## Choosing `shift`

Ticks scale exactly with 2^N (measured, same workload):

| shift | ticks | instructions/tick | wrap ceiling |
|---|---|---|---|
| 0 | 560 | ~40 | ~670M instructions |
| 3 | 4482 | ~5 | ~84M |
| 5 | 17930 | ~1.1 | ~16.7M |
| 6 | 35859 | ~0.6 | ~8.4M |

`shift=5` is the default: ~1 tick per instruction, near-exact counting. Trade it
for headroom when tests hit the wrap ceiling (see below).

Absolute counts scale with 2^N, so rows measured at different shifts are **not
comparable** — `qemu_cycles.shift` records it and `compare_worktree.py` keys its
baseline cache on it, so switching shift re-measures rather than diffing
incompatible units.

Counts are an instruction-retired proxy, not silicon cycles — no pipeline or
memory stalls are modelled. That is what makes them deterministic, and it is
the right property for A/B optimizer comparison. For real cycles use the
RP2350 hardware benchmark (`tests/benchmarks/run_benchmark.py`).

## Limitation: 24-bit wrap

SysTick is a 24-bit down-counter. The shim samples only at start and exit, and
`COUNTFLAG` records *at most one* reload, so a test exceeding the ceiling above
would **silently undercount** — a failure that reads as an improvement. Rather
than record that, `qemu_corpus.py` leaves the count NULL for any test at or over
the ceiling and says so. The test's functional verdict still stands — only its
cycle count is withheld.

This is not theoretical, and it bites the tests you most want to profile: at
the default `shift=5`, `ir/mibench_dijkstra` runs 28.3M instructions and is
dropped. `--cycles-shift 0` buys 40x headroom (measuring it at 884,240 ticks —
consistent, since 884,240 x 32 = 28.3M) and covers 379/379 of the `ir` suite,
at ~40 instructions per tick.

The real fix is to count wraps in a SysTick ISR, which needs
`.weak SysTick_Handler` in `boot.S` (`def_irq_handler` emits strong symbols
today) so the shim can own the vector.

## Validation

`108_loop_unroll_basic`, tcc, shift=5, byte-identical across repeated runs:

| level | cycles |
|---|---|
| -O0 | 3062 |
| -O1 | 2768 |
| -O2 | 2746 |

## Gotcha: never hand QEMU the terminal

Spawn QEMU with `stdin=DEVNULL`. `-nographic` wires the serial to stdio, and if
fd 0 is a tty QEMU switches it to raw + `O_NONBLOCK` for the duration. Because
fds 0/1/2 share one open file description, that makes the *caller's stderr*
non-blocking too: concurrent writes then die with
`BlockingIOError: [Errno 11] write could not complete without blocking`, far
from the cause. QEMU restores the tty on a clean exit — but not when killed, so
one timed-out test leaves the terminal raw and every later write fails.

`capture_output=True` does not cover this: it redirects stdout/stderr and leaves
stdin inherited. A non-tty stdin (CI, most harnesses) hides the bug entirely,
which is exactly how it reaches an interactive terminal unnoticed. If a run did
wedge your terminal, `stty sane` or `reset` restores it.

## Gotcha: the shim must be compiled at -O0

The shim reads MMIO through file-scope `volatile T *const` pointers. At `-O1`
and above const-propagation folds the constant address into the dereference,
which hits the constant-address deref miscompile (see `docs/bugs.md`) and makes
every read return the same constant — `##CYCLES 0`. Compiling the shim as its
own translation unit at `-O0` avoids this; its own code quality is irrelevant
to the measurement, and its overhead is constant and cancels in a diff.
