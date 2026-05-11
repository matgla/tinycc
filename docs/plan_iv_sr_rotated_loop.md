# Plan: IV Strength Reduction for Rotated Loops with `arr[i*const]`

## Context

`test_llong_relops::run_signed` and `run_unsigned` are ~1.39x and ~1.41x larger
than GCC's output (139 vs 100, 128 vs 91). The gap is dominated by:

1. The loop counter `i` is spilled to `[sp, #36]` and the address
   `&cases[i]` is recomputed each iteration via `mla r9, r0, r1, r2`.
2. GCC instead uses a pointer-IV: `r4 = &cases[0]` in the preheader,
   `r4 += 40` in the latch, eliminating both the multiply and an `i` reload.

TCC already has an IV strength reduction pass
([`tcc_ir_opt_iv_strength_reduction`](ir/opt.c:20889)) that's designed for
exactly this pattern — but it doesn't fire in `test_llong_relops`. This plan
covers what blocks it and how to fix it.

## Root Cause

The fix has two distinct blockers. Either one alone keeps the pointer-IV
transform from firing.

### Blocker 1: pre-SSA MLA fusion rejects immediate multipliers

[`tcc_ir_opt_fusion_pass`](ir/opt.c:14461) fuses `T = a * b; V = base + T`
into `V = a MLA b + base`. The gate at [ir/opt.c:14523-14524](ir/opt.c#L14523)
excludes the case where `a` or `b` is an immediate:

```c
!irop_is_immediate(ms1) && !irop_is_immediate(ms2) && ir_opt_du_uses(...) == 1
```

For `T = i * 40; V = base + T`, `ms2` is `#40` (immediate), so MLA fusion
skips it. The MUL+ADD form survives until the ARM-specific SSA-stage MLA
fusion in [`arch/arm/ssa_opt_arm.c:100`](arch/arm/ssa_opt_arm.c#L100) — but
**that runs after IV-SR**, so IV-SR never sees an MLA to operate on.

The pre-SSA gate was presumably added because MUL-by-power-of-2 gets
strength-reduced to SHL later, which would render the MLA wasteful. But for
non-power-of-2 immediates (40, 12, etc.) the strength reducer at
[ir/opt.c:18846](ir/opt.c#L18846) bails out (multi-instruction patterns
aren't supported), so the MUL stays as MUL and MLA fusion was the right call
all along.

### Blocker 2: `loop->body_instrs` is too narrow for TCC's rotated layout

`find_derived_ivs` ([ir/opt.c:19115](ir/opt.c#L19115)) has two scan passes:

| Pass | What it finds | Scan range |
|------|---------------|------------|
| 1 (line 19164) | `ADD` with MUL/SHL src — i.e. unfused MUL+ADD | `loop->body_instrs` |
| 2 (line 19400) | `MLA` directly | `mla_scan_start..mla_scan_end` (extended) |

The extended range walks forward jumps iteratively past the back-edge — it's
specifically designed to catch rotated loops with the body proper *after* the
latch in instruction order. But it's only wired to pass 2 (MLA-detection).

In `test_llong_relops`, loop rotation produces:

```
op  3: CMP i, 10            ← header
op  4: JMP if >=U  exit
op  5: JMP to 10            ← into body
op  6: T = i + 1            ← latch (increment)
op  8: i = T                ← latch (write-back)
op  9: JMP to 3             ← back to header
op 10: T3 = i * 40          ← body proper (MUL)
op 11: V1 = base + T3       ← body proper (ADD) — this is the DIV!
...
op 110: JMP to 6            ← back-edge to the latch
```

LICM's body detector ([ir/licm.c:228-264](ir/licm.c#L228-L264)) only follows
forward jumps one level deep when extending the body range, so
`loop->body_instrs` for this loop is `{2, 3, 4, 5, 6, 7, 8}` — it never
reaches op 11. Pass 1 misses the MUL+ADD.

Even after fixing Blocker 1 (so the MUL+ADD becomes an MLA), Pass 2 catches
it because Pass 2 uses the extended scan range.

## What I Tried — and Why It Failed

Lifted the immediate-operand gate on pre-SSA MLA fusion. IV-SR then *did*
fire and produced the textbook pointer-IV in the IR dump:

```
0002: R4(T27) <-- Addr[StackLoc[-48]] [ASSIGN]    ← preheader: p = base
...
0013: R4(T27) <-- R4(T27) ADD #12                  ← latch: p += stride
```

But the **emitted assembly didn't match the IR**:
[`bug_struct_array_index_mul_clobber`](tests/ir_tests/bug_struct_array_index_mul_clobber.c)
crashed in QEMU because `main`'s emitted code loaded from `[r4, #0]` without
ever initializing r4. The preheader `ASSIGN R4 <- Addr[...]` was in the IR
but absent from the machine code. The latch `R4 += 12` was also missing.

So there's a third blocker hiding behind the first two: when IV-SR inserts
new instructions *outside the original loop range* (specifically into the
preheader/latch), something in the codegen path doesn't pick them up.

I reverted the MLA fusion change. The peephole improvement in commit
`e76cee04` (which is an unrelated, smaller win) stands.

## The Real Fix

Three changes, in order. Land each on its own commit and run the full IR
suite (1026 tests) plus a regression-disasm diff between each.

### Step 1 — Verify and fix the codegen-doesn't-honor-inserted-instructions bug

Without this, Steps 2-3 produce miscompiles.

1. Reproduce with a minimal case. Apply the immediate-allowing MLA fusion
   from this session (`git show e76cee04^..HEAD` is the wrong base — apply
   the change as a separate scratch commit). Compile
   `tests/ir_tests/bug_struct_array_index_mul_clobber.c` with `-O2 -dump-ir`.
   The "AFTER OPTIMIZATIONS" IR dump for `main` will show
   `R4(T27) <-- Addr[StackLoc[-48]]` near the top and `R4 += 12` in the
   latch.
2. Confirm the disassembly is missing both: there's no `add r4, sp, #N` in
   `main`'s preheader and no `adds r4, #12` in the loop's bottom block.
3. Hypothesis: IV-SR's `transform_derived_iv`
   ([ir/opt.c:~19500](ir/opt.c) — search for it) inserts via
   `insert_instr_at` at `loop->preheader_idx + 1` and at the latch position.
   Those inserts shift indices. Either:
   - the inserts land in an IR slot that codegen skips (NOP-classified, or
     marked unreachable), or
   - the inserts happen *after* the SSA-renaming snapshot codegen uses, and
     codegen runs from the pre-IV-SR snapshot.
4. The way to find out is to instrument `tcc_ir_codegen_generate` to print
   `(i, op, dest_vreg, dest_alloc.r0)` for every IR instruction it dispatches
   on, and compare against the dumped IR. The first divergence is the bug.

Most likely fix is in `transform_derived_iv` (it needs to mark new
instructions with the right flags), or in the SSA construction pass (it
needs to rebuild after IV-SR runs). Don't guess — the trace will say.

### Step 2 — Relax pre-SSA MLA fusion to accept non-power-of-2 immediates

Once Step 1 is done, re-land the immediate-allowing MLA fusion. The patch
in [ir/opt.c:14523](ir/opt.c#L14523):

```diff
+ int ms1_imm = irop_is_immediate(ms1);
+ int ms2_imm = irop_is_immediate(ms2);
+ int allow_one_imm = (ms1_imm ^ ms2_imm);
+ if (allow_one_imm) {
+   int64_t mval = ms1_imm ? irop_get_imm64_ex(ir, ms1)
+                          : irop_get_imm64_ex(ir, ms2);
+   if (is_power_of_2(mval) >= 0 || mval == 0 || mval == 1)
+     allow_one_imm = 0;  /* leave for strength reduction */
+ }
  if (... &&
-     !irop_is_immediate(ms1) && !irop_is_immediate(ms2) && ...) {
+     (allow_one_imm || (!ms1_imm && !ms2_imm)) && ...) {
```

Forward-declare `is_power_of_2` near the top of `ir/opt.c`.

Do **not** also drop the `STACKOFF && !is_lval` accumulator exclusion. That
exclusion is load-bearing (dropping it breaks `test_llong_relops` and
`bug_bitfield_packed10` in different ways — distinct from Step 1's bug).

### Step 3 — Optional: extend Pass 1 of `find_derived_ivs` to the MLA scan range

After Step 2, the test_llong_relops MUL+ADD becomes an MLA in pre-SSA, so
Pass 2 catches it. But other callers / code shapes may still have unfused
MUL+ADD outside `body_instrs`. The cleanest follow-up is to teach Pass 1 to
walk `mla_scan_start..mla_scan_end` as well, gated to only consider ADDs
whose matched MUL/SHL is *also* in the extended range. This preserves the
"don't extend body for SHR/AND chains" guarantee the comment at
[ir/opt.c:19126-19131](ir/opt.c#L19126-L19131) warns about.

This is genuinely optional — Step 2 alone should close the test_llong_relops
gap once Step 1 is in place.

## Expected Impact

| Function | Before | After Steps 1-2 | GCC |
|---|---|---|---|
| `test_llong_relops::run_signed` | 138 | ~115 (-23) | 100 |
| `test_llong_relops::run_unsigned` | 127 | ~104 (-23) | 91 |
| (`bug_ull_mul10_loop`, others with `arr[i*c]`) | — | likely improves | — |

The 23-instruction estimate per function comes from:
- Eliminate `mla r9, r0, r1, r2` plus its prep (`movs r1, #40; add r2, sp,
  #40`) per iter → -3 insns in body, but body executes ×10/8 → counted as
  static body shrink.
- Eliminate `i` spill (`str/ldr` to `[sp, #36]` ~6 times per iter once `i`
  fits in a callee-saved reg, since one register is freed by the IV-SR
  collapse) → ~6 insns gone from body.
- Net ~9 insns saved in the body, plus 14 in the prologue/preheader once the
  computed-each-iter MLA collapses to a single preheader init + latch ADD.

This won't close the gap entirely (GCC also uses cleaner long-long
relational comparisons — `sbcs`/`ite` patterns that TCC already produces but
spills around for the last comparison; see todo #3 from the original
analysis: `ne_s`/`ne_u` regalloc collision).

## Out of Scope

- The regalloc collision causing `ne_s`/`ne_u` to spill `got` and `exp` to
  `[sp, #32]`/`[sp, #28]` (separate fix, ~6-8 insns).
- The dead intermediate `[sp, #24]` store from `i++` (would require DSE on
  the post-codegen stack slot, or IR-level coalescing of T54 with T51).
- LICM body detection fix in `ir/licm.c` (a more thorough fix to Blocker 2
  but with broader regression surface — Step 3 above is the targeted
  alternative).

## Validation

Per step:

```bash
make cross
cd tests/ir_tests && source .venv/bin/activate
python -m pytest test_qemu.py -n auto                     # 1026 tests must pass
cd /home/mateusz/repos/tinycc
python scripts/regression_disasm.py --suite=ir -O2        # check function-level deltas
```

Specifically watch:
- `test_llong_relops::{run_signed,run_unsigned}` (target test)
- `bug_struct_array_index_mul_clobber::main` (Step 1 canary)
- `bug_bitfield_packed10::{check,main}` (was broken by dropping STACKOFF
  exclusion — must stay passing)
- `110_iv_strength_reduction::*` (existing IV-SR test surface)
