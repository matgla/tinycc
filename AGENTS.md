# AGENTS.md

Guidance for autonomous coding agents working in this repository (TinyCC fork
targeting ARMv8-M). Read this first, then `CLAUDE.md` for the full project
overview, build commands, and architecture.

## Build & test (always current)

```bash
make cross -j$(nproc)                       # build armv8m-tcc (rebuild after EVERY edit)
make test -j16                              # IR test suite (primary gate)
python3 scripts/diff_olevels.py --seeds 0-5000 --require-qemu   # fuzz self-consistency
```

Style: `-std=c11 -Wunused-function -Werror` (treat warnings as build failures).
Function-body brace on its own line; see `.clang-format` and `CLAUDE.md`.

## Debugging an optimizer miscompilation

When a fuzz seed diverges between O-levels (tcc -O0 correct, -O1/-O2 wrong),
follow **`docs/debugging_fuzz_divergences.md`** end-to-end:

1. `scripts/bisect_opt.py --seed N --high=-O1` — QEMU-confirms the culprit
   knob(s) and flags the exact IR line where a memory read is misfolded to a
   constant, naming the pass group and the gated pass functions.
2. Write a **regression test first** (`tests/ir_tests/NN_fuzz_<cause>.c` +
   `.expect`, registered in `tests/ir_tests/test_qemu.py`); confirm it fails
   before the fix and passes after.
3. Fix, rebuild, re-run the IR suite + a fuzz sweep; confirm zero *new*
   divergences.

Ground truth oracle is `gcc -m32 -funsigned-char` (ARM ABI: unsigned char,
32-bit long). Sweep/triage infrastructure is documented in
`docs/fuzz_triage_guide.md`.

## Conventions for changes

- **Never commit without a regression test** for a bug fix — verbatim or reduced
  repro under `tests/ir_tests/`, expected output in a `.expect` file.
- New IR opcode → lowering in `arm-thumb-gen.c` + test. New asm instruction →
  builder in `arm-thumb-opcodes.c` + token + parser + test.
- IR internals live in `ir/` (included via `ir/ir.h`); the public IR interface
  is `tccir.h`. Internal IR functions are `ir_<module>_<action>()`.
- Don't commit the temporary `TCC_SKIP_SSA*` env-var bisection gates (see the
  triage guide); they are investigation-only scaffolding.

## Don't

- Don't disable ASan/leak checks to "fix" a failure; investigate the root cause.
  (ASan is ON by default; `./configure --disable-asan` for fast builds only.)
- Don't commit secrets, force-push, or create empty commits.
