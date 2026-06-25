# Plan — speed up device tcc by closing the inlining gap

Companion to [tcc_vs_gcc_O2_codegen_report.md](./tcc_vs_gcc_O2_codegen_report.md). Goal: cut
device compile CPU by inlining the hot `static inline` helpers tcc currently emits out-of-line.

## Facts the plan is built on

- tcc has **no C-function inliner**; `static inline` → one out-of-line copy per TU, never inlined.
- `IROperand` is **9 bytes**, passed/returned **by value** → every accessor call does an sret
  struct copy + table lookups + bounds checks, and none of it CSEs across calls.
- Call-site counts (the leverage): `irop_get_vreg` **1351**, `tcc_ir_op_get_src1` **924**,
  `tcc_ir_op_get_dest` **871**, `tcc_ir_op_get_src2` **557**, `irop_make_imm32` **175**.
- The accessors are **branchy / multi-statement** (table lookup + bounds guard + sentinel
  handling) — so they are *not* trivially macro-izable; a real inliner or careful
  statement-expression macros are needed.
- `tccpp.c` (lexer/preprocess) is **~60% of compile CPU**; the IR accessors dominate the backend.

## Build/validation harness (applies to every phase)

- **★ Clean rebuild after header edits.** The tinycc Makefile has no header dependency tracking;
  editing `tccir_operand.h` / `tccir.h` / `tcc.h` requires `rm *.o ir/*.o ir/opt/*.o` (or
  `make distclean`) or you get stale-object SEGVs. (Known gotcha, see memory.)
- **CPU measurement:** `scripts/tcc_profile.py -n 30` (device-representative `Ir`), plus
  `--save`/`--compare` for before/after deltas. Also profile `-O1`/`-O2` compiles, not just `-O0`.
- **Size:** `arm-none-eabi-nm -S bin/armv8m-tcc.elf` totals + per-helper copy counts.
- **Correctness:** QEMU smoke suite (must stay 412 pass / 0 undefined) + the tcc test suite;
  confirm self-host rebuild is byte-stable (cross-built tcc and self-built tcc agree).

## Phase 0 — Validate the lever (½ day, throwaway branch, no compiler change)

Prove the predicted win before investing in an inliner.

1. Force-inline the single hottest cluster only — `tcc_ir_op_get_src1/src2/get_dest` +
   `irop_get_vreg` — by rewriting them as GNU statement-expression macros (`({ ... })`, which tcc
   supports) **or** `__attribute__((always_inline))` if tcc honors it (check first; likely not).
2. `rm` objects, rebuild the **cross** `armv8m-tcc` (x86), re-run `scripts/tcc_profile.py
   --compare base.json` on `129_scopes.c` at `-O0` and `-O1`.
3. **Decision gate:** if total `Ir` drops materially (expect several %), continue to Phase 1.
   If not, the cost is elsewhere (struct-by-value ABI, table lookups) → pivot to Phase 1-B.

Capture `base.json` from the *current* tree first so the comparison is honest.

## Phase 1 — Pick the implementation path (decision gate after Phase 0)

### Path A — minimal inliner in tcc (preferred if Phase 0 win is broad)
Highest leverage, compounds (an inlining tcc builds a faster tcc), fixes the 226 KB duplication
too. Higher risk given this fork's history of self-host miscompiles — so keep it **conservative
and gated**:
- Inline only functions that are: marked `inline`/`static inline`, single `return` or
  straight-line + ≤1 branch, below an IR-instruction-count threshold, non-recursive, no varargs,
  no address-taken. Everything else untouched.
- Implement at the IR/frontend boundary (where call lowering happens), behind a flag
  (`-finline` / config define) defaulted off until validated, so it can be bisected like every
  other opt pass in this tree.
- Validate with the full self-host + QEMU loop after **every** increment.

### Path B — targeted, no new pass (fallback / lower risk)
- Macro-ize (statement-expression) the top ~8 hottest accessors from the report:
  `irop_get_vreg`, `irop_set_vreg`, `tcc_ir_op_get_src1/2`, `tcc_ir_op_get_dest`, `irop_get_tag`,
  `irop_make_imm32`, `irop_init_phys_regs`.
- **Plus** the orthogonal ABI win: change the worst by-value-9-byte-struct accessors to take
  `const IROperand *` / write through an out-pointer, killing the sret copy even where inlining
  doesn't reach. (Invasive across call sites — script the rewrite, do one accessor at a time.)
- Do the lexer helpers too (`cstr_ccat`, `tok_str_add2`, `token_lookup_cache_find`,
  `default_reallocator`) — they sit in the 60%-CPU bucket.

Recommendation: **start Path B** (safe, incremental, immediately shippable), and pursue Path A
only if Phase 0 shows the general inliner is worth the miscompile risk.

## Phase 2 — Correctness & stability

- QEMU smoke 412/0; tcc suite green; self-host byte-stability check.
- Watch for the known traps: stale-object SEGVs (clean rebuild), `build_rootfs.sh` not
  fail-fast on cross `-Werror` (grep build.log for `error:`), statement-expression macros
  double-evaluating arguments with side effects (audit each macro's args).

## Phase 3 — Measure, report, decide next lever

- Before/after: profiler `Ir` (total + per-fn), `.text` size, helper copy counts, and a real
  device compile-time round-trip on a representative source.
- Update the report with measured deltas. Next lever after inlining is the §4 +19% codegen
  quality (jump tables for dense enum switches, machine-level CSE of struct-field reloads).

## Deliverables checklist

- [ ] `base.json` profiler baseline committed/saved
- [ ] Phase 0 experiment branch + measured `Ir` delta
- [ ] Path decision recorded (A vs B) with the numbers behind it
- [ ] Implementation behind a flag, validated incrementally
- [ ] QEMU smoke + self-host stability green
- [ ] Report updated with before/after
