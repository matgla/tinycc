# Next-session prompt — tinycc O1/O2 fuzz miscompile hunt

Continue the tinycc O1/O2 differential-fuzz miscompile hunt in
`/home/matgla/repos/yasos.zig/libs/tinycc` (branch `heapOverflowBug`). START by reading the
memory file `yasos-tinycc-fuzz-divergence-playbook` (the per-seed investigate→fix→regression-test
loop); it and the per-seed memories auto-load via MEMORY.md.

## Golden rules
- tcc -O0 is the trusted oracle; ground truth = `gcc -m32 -funsigned-char` (unsigned char, 32-bit long).
- After EVERY compiler edit: `make cross -j$(nproc)`.
- Single repro: copy `seedN.c` into `tests/ir_tests/`, then `python run.py -c seedN.c --cflags="-O1"`
  (grep `checksum=`). Reproduce/confirm with `python3 scripts/diff_olevels.py --seed N --require-qemu`.

## Workflow per seed
1. Reproduce; note failing level + correct O0 checksum.
2. Bisect the culprit pass with `TCC_DISABLE_PASS=<name>` (works for opt-pipeline AND `ssa:<name>`
   passes now). Names: `grep PASS_GATED ir/opt_pipeline.c` and the RUN_SSA list in
   `ir/regalloc.c` / `ir/opt/ssa_opt.c`.
3. **TRIGGER ≠ ROOT**: the `-fno-*` knob (esp. `-fno-const-prop`) is usually a TRIGGER — a *sound*
   pass (const-prop of a genuine constant, a sound DSE) reshapes the IR and exposes a downstream
   bug. Several passes "fixing" it when disabled = enablers; keep bisecting to the pass that
   CREATES the wrong value.
4. Pinpoint within a pass via a STABLE skip-by-vreg/offset knob (vregs persist across pass
   iterations; instruction indices do NOT) + a debug log, then bisect. Proven knobs:
   REDKILL_KEEPVAR (redundant_var_assign), CPA_SKIP_DEST (cprop), SR_KEEPOFF (store_redundant),
   SLF_SKIP_DEST (sl_forward). SSA phi nodes are NOT shown by `-dump-ir`; dump
   `ctx->ssa->block_phis` manually if a phi is involved.
5. **Do NOT printf to isolate the divergent variable** — it perturbs opt and misattributes. Map a
   VAR to a source var by its init constant in the earliest IR dump
   (`run.py --dump-ir-passes=all --cc-output`).
6. Fix conservatively. Add `tests/ir_tests/NN_fuzz_<cause>.c` + `.expect` (the gcc value), register
   in `TEST_FILES` in `tests/ir_tests/test_qemu.py`. PROVE fail-unfixed (toggle the fix to `if(0)`),
   pass fixed at O0/O1/O2/Os.
7. Validate: `python -m pytest test_qemu.py -n 16 -q`; diff_olevels sweep over triage seeds (no NEW
   divergences); then `make test -j16` MUST be green (unit tests + self-host gate + IR pytest).
   Commit (end message with the Co-Authored-By line).

## Done this session (committed, all make-test-green)
- `d528cd9d`: 2137 + 8425 (ARM `fuse_store_src_through_add_imm` load hoist across store);
  2657 (`load_cse` runtime stack-indexed store invalidation). Tests 210, 211.
- `020964a3`: 2698 + 5689 + 8300 + 8606 (`cprop_assign` lost-copy into loop back-edge phi). Test 212.
- `ec9128db`: 2874 (`store_redundant` constant-index LOAD_INDEXED read eviction). Test 213.

## Next target — seed 3210
`-O1`, O0=`a720d0d4` vs O1/O2=`2c0f55a4`. Read memory `yasos-tinycc-seed3210-slforward-open`.
Localized to `sl_forward` (NOT store_redundant), load T194 / dest 536871106 (sl_forward-time i=198):
skipping its forward fixes it, but the stack-local forward-match never fires there → subtler
mechanism. Re-add SLF_SKIP_DEST, dump IR with the skip (minimal-correct) vs buggy and diff T194's
region; or log every forward-commit site in `tcc_ir_opt_sl_forward` (ir/opt_memory.c) with the dest.

## Other open seeds (each likely a separate root)
4482, 5656, 6214, 6447, 9403 (`-fno-const-prop` O1); 4193, 4594, 7918 (no knob);
6951 (`-fno-jump-threading`); 8985 (`-fno-loop-unroll`);
8078 (COMPILE_CRASH: `STORE operand produced MACH_OP_NONE`). Triage table: `fuzz_triage_2000_10000.md`.

## Gotcha
`tests/benchmarks/libs/pico-sdk` is an external checkout — never `git add` it. Commit with
`git add -A -- ':!tests/benchmarks/libs/pico-sdk'`.
