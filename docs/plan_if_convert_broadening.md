# If-conversion broadening — staged plan

Goal: close the tcc-vs-gcc codegen gap on ternaries / `min`/`max`/`abs`/`clamp` /
`if(c) x=y;` by turning more control-flow diamonds into branch-free `SELECT` (IT
blocks), and lowering the common cases to a single predicated instruction.

## Where the code lives (done — Stage 0)

The if-conversion family was extracted from the 2152-line `ir/opt_promote.c`
grab-bag into the flat landing zone:

- `source/opt/flat/cfg/if_convert.c` — `tcc_ir_opt_select`,
  `tcc_ir_opt_setif_neg_to_select`, `tcc_ir_opt_post_ra_forward_diamond`
- `source/opt/flat/include/opt/flat/if_convert.h` — declarations

Drivers/callers unchanged: pre-RA `select`/`setif_neg` from
`source/opt/function_pipeline.c`, post-RA `post_ra_forward_diamond` from
`source/backend/generators/regalloc.c`. Byte-identical relocation
(772/772 objects, `make test-ir` 13614 zero-delta, UT 2679).

Engine decision: **hand-written C, not the peephole DSL** — these are
whole-function CFG scans with `jt_cnt` jump-target bookkeeping and multi-block
matching, which `PATTERN`/`GUARD`/`REWRITE` (single instruction `i`) can't
express. Same call as `cmp_eq` on the SSA side.

## The gap (measured, armv8m-tcc -O2 vs arm-none-eabi-gcc -O2 cortex-m33)

| fn | tcc | gcc | why tcc loses |
|----|-----|-----|---------------|
| `abs2` `x<0?-x:x` | 7 | 4 | diamond kept as branches; computed arm (`rsb`) + trailing merge-ASSIGN rejected by matcher |
| `clampu` `x>255?255:x` | 7 | 4 | one arm is a plain variable (LOAD of param) — rejected; other is const |
| `sel` `x>0?5:-3` | 5 | 5 | already SELECT (both arms const) — matcher's supported case |

`tcc_ir_opt_select` today only accepts arms that are a single `ASSIGN` /
`LOAD-of-immediate` of a **const/symref**, both arms the same op, writing the
same dest vreg. Everything else stays a branch diamond.

## Stage 1 — variable / register arms  (DONE, commit c61aacf8)

Broadened the "Simple ASSIGN diamond" matcher (`if_convert.c`,
`tcc_ir_opt_select`) to accept a `LOAD` arm whose value is a plain non-lvalue
register/param (`x>c?x:k`), via the new `ir_ifconv_arm_value_safe` predicate:

- immediate → always safe (unchanged behavior)
- lvalue deref (`is_lval`, the `***DEREF***` dump marker) → rejected (a
  pointer/global read may fault on the not-taken path once hoisted)
- **float/double variable → rejected** (LEARNED: `pr39501` min/max regressed —
  an FP compare lowers to a soft-float **CALL** whose condition/flags the SELECT
  lowering does not reproduce; FP select is its own stage). Immediate FP arms
  stay allowed (they already worked).

`clampu` `x>255?255:x`: 7 → 4 insns (`cmp; it hi; movhi; bx`) — the SELECT
lowering already contracts the identity arm to one predicated move (the Stage 3
effect, for free, when an arm equals the input reg).

Result over ir_tests+torture (-O1/-O2): 18 smaller, 2 larger (+4/+8B loop-body
diamond → branch-free ite/mov pair, accepted speed/size tradeoff), −476B net.
UT 2681/0 (+2), test-ir 13614/0. Existing guards (side-effect-free, same dest,
single JUMP, `jt_cnt` no-other-pred) preserved.

## Stage 2 — computed arms + trailing merge-ASSIGN  (fixes `abs2`, `cond_add`)

Accept an arm whose block is a single side-effect-free compute
(`SUB`/`ADD`/`RSB`/`AND`/`OR`/`SHL`/…) producing the arm value, plus the
frontend's `Tmerge <-- Tarm [ASSIGN]` merge form (see the abs2 IR dump: then-arm
`T0 <-- #0 SUB P0`, merge `T1 <-- T0`). The computed value is materialized
unconditionally before the `SELECT`. This makes `abs2` **branch-free** (IT-based)
even before Stage 3, though not yet minimal.

- Reuse `ir_ifconv_arm_value_safe` for the compute's operands, and keep the FP
  exclusion (Stage 1 learning): a float/double arm whose condition is a
  soft-float compare CALL must not convert until FP select is handled.
- Only pure ALU ops (no LOAD/STORE/CALL/DIV-by-maybe-zero) may be speculated.

- Structural change: the matcher currently keys on "arm == single ASSIGN/LOAD";
  generalize to "arm block = optional single pure compute + the value-defining
  op", tracking the produced vreg through the trailing merge ASSIGN.
- Keep the strict single-def / single-use / no-other-pred guards.
- UT: `x<0?-x:x`, `x>10?y+2:y`, `c?a&m:a`.

### Stage 2 status (DONE — commits dc38e4df, 680b1a1d)

- **2a (IR-linearize, dc38e4df):** the merge-temp matcher landed. `abs2` 7→6.
  Net −654B (22 smaller, 0 larger). Correctness fix: **64-bit computes
  excluded** — a 64-bit ALU op is a SUBS/SBCS carry chain forced flag-setting,
  clobbering the CMP flags (llabs miscompiled; caught by test-ir). 32-bit
  `rsb.w`/`sub.w` is flags-safe, so the hazard is auto-handled by `flags_safe()`.
- **2b else-identity (680b1a1d):** `tcc_gen_machine_select_mop` elides a
  `movXX dest,dest` when the else-arm is already in dest (mirror of the existing
  then-identity shortcut). `abs2` 6→5. Net −52B.
- **predicate-the-compute (→ gcc's 4) — NOT DONE / deferred.** Fuse
  `rsb r1,r0,#0; movlt r0,r1` into `it lt; rsblt r0,r0,#0` via a CMP-peephole
  predicating the compute inside the IT block. High effort/risk (IT-block
  predicated-ALU emission, cf. ptr-5759) for one instruction on abs-like shapes.

## Stage 3 — backend: identity-arm predication  (abs2/clampu -> gcc parity)

In the `SELECT` lowering (`ir/codegen.c:4068` → `tcc_gen_machine_select_mop`),
when one arm already equals the dest/input register (identity), emit a **single
predicated instruction** under a 1-slot IT instead of `ite; mov; mov`:

- `clampu`: else-arm = x (in r0) → `cmp; it cs; movcs r0,#255`
- `abs2`:   else-arm = x (in r0), then-arm = `-x` → `cmp; it lt; rsblt r0,r0,#0`

This is a localized backend peephole on the already-formed `SELECT`; it needs
the identity-arm recognition (arm operand == the value already live in dest) and
a predicated form of the other arm's op (mov-imm, rsb, add-imm, …). Reuses the
existing IT-window machinery (`arm-thumb-gen.c` IT emission, guarded per
[[ptr-5759-pool-flush-inside-it-block]]).

## Stage 4 (optional, only if Stages 1-2 hit a ceiling) — SSA if-conversion

If the flat matcher can't reach multi-statement arms / arbitrary block shapes,
reconsider a phi→select pass under `source/opt/ssa/cfg/`. Bigger lift (SSA-cfg
harness has a dom-frontier gap per [[cmp-eq-cfg-pass-relocation]]); defer until
the flat broadening demonstrably plateaus.

## Validation gate (every stage)

`make cross` clean `-Werror`; assert-first UT repro then close; `make test-ir`
zero-regression; byte-delta objdump sweep over ir_tests + torture (the
`grep -v 'file format'` harness) to confirm **net improvement, no unexpected
diffs**. Per no-fuzz-runs-user-verifies: user runs the fuzzer.
