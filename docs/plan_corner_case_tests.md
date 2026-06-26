# Plan — corner-case unit tests for the optimizer passes

**Priority (per user, 2026-06-26): do this FIRST**, before the gcc differential (Track 3) and IR
metamorphic fuzzer (Track 4) in `docs/plan_bug_hunting.md`. The 11 integrated suites (Phase B,
`PASS_COVERAGE.md`) cover the happy path + a few guards; this phase systematically drives each pass
into its **edge cases**, where miscompiles actually hide.

## Principle — make corner cases semi-oracles, not characterization
Where a corner case has an **implementation-independent expected result**, assert that
independently-computed value (not "what the code does"). Then the test can actually *find a bug*:
- e.g. constant-fold of `INT_MIN / -1`, `x << 32`, `INT_MIN` negation, `(uint8_t)0x1F2` — the
  correct result is dictated by C/ARM semantics, computed in the test, **not** read from the pass.
- For structural passes (jump-thread, licm, dce) the oracle is an invariant (semantics preserved,
  no out-of-range target, converges) rather than a numeric value.

Rules unchanged: **no production edits**; a confirmed wrong result → *Findings* in `PASS_COVERAGE.md`
with a minimal repro, and assert the **correct** value only if it keeps the suite green; if the pass
is actually wrong, assert current behavior + `/* SUSPECTED BUG */` + Finding (so `make ut` stays green).

---

## A. Cross-cutting corner-case checklist (apply to every pass)
1. **Integer boundaries**: 0, 1, -1, `INT_MIN`, `INT_MAX`, `UINT_MAX`, sign-bit set/clear, powers of two.
2. **Overflow / UB-shaped inputs**: ADD/SUB/MUL 32-bit overflow; `INT_MIN` negation; shift count 0 / 31 / 32 / ≥width / negative; `INT_MIN / -1`; div/mod by 0 (pass must fold-correctly or bail, never crash).
3. **Width & signedness**: INT8/INT16/INT32/INT64; `is_unsigned` on/off; narrow→wide and wide→narrow; mixed-width operands (the historical byte-drop class); zero/sign-extension boundaries.
4. **Degenerate IR**: empty fn; single instruction; all-NOP; NULL ir; max temps (interval-table growth); repeated identical ops.
5. **Control flow** (passes that care): merge point (≥2 preds); back-edge / loop; jump-to-self; target at 0, n-1, and out of range; unreachable block; fallthrough into a labeled target.
6. **Lvalue / memory**: `is_lval` set on dest / src1 / src2 independently; `is_llocal` double-indirection; STORE/LOAD alias (same / overlapping / disjoint byte ranges); address-taken / escaped.
7. **Operand kinds per slot**: immediate vs vreg vs symref vs stackoff; the 4th-operand ops (LOAD_INDEXED/STORE_INDEXED scale, MLA accum, SELECT cond).
8. **Idempotence / fixpoint**: second run returns 0; pass converges; metadata preserved (`orig_index`, `operand_base`, `is_jump_target`, `line_num`).
9. **Robustness**: runs under ASAN clean on all of the above (the suite binary should be built/run under ASAN in CI for this phase).

---

## B. Harness extensions this phase needs (do these before/with the fan-out)
Small additions to `ir_build.h` / a shared helper — needed because corner cases use shapes the current
builder doesn't expose:
- [ ] `utb_emit4(ir, op, dest, src1, src2, op4)` — write the 4th operand at `operand_base+3` (LOAD_INDEXED/STORE_INDEXED scale, MLA accum, SELECT cond) and bump the pool count.
- [ ] `utb_symref(...)` and `utb_stackoff(off, is_lval, is_llocal, is_param, btype)` constructors (for symref_const_prop, dead_lea_store, param/local cases).
- [ ] flag helpers: `utb_lval(op)`, `utb_unsigned(op)`, `utb_llocal(op)` returning a modified copy (cleaner than post-hoc field writes).
- [ ] `utb_run_to_fixpoint(ir, passfn)` — apply until it returns 0; assert convergence.
- [ ] **Settable `get_tok_str` table** (resolves Finding #1): a test-populated token→name map so name-gated constfold passes (`self_copy_elim`, `float_narrowing`, `*_string_calls`, `*_call_replace`) can reach their real positive fold. Replace the constant `"?"` stub with one that reads the table.
- [ ] (optional) a tiny structural sanity checker `utb_assert_wellformed(ir)` — operand counts vs `irop_config`, vreg in range, jump targets in [0,n) — call after each pass in every corner-case test.

---

## C. Per-pass corner-case enumeration
Each item = one (or a few) new test(s) added to that pass's existing `test_opt_*.c`. ★ = likely
bug-revealing semi-oracle (assert an independently-computed value).

**neg_chain** — chain length 1/2/3/N; ★mixed-width links (INT8↔INT32) must not fold; merge-point reset mid-chain (loop) clears canon; SUB with non-zero minuend; non-temp / VAR operands; reuse after reset; idempotence.

**known_bits** — AND with 0 / all-ones / partial mask; OR with 0 / all-ones; XOR self; ★SHL/SHR/SAR by 0/31/32 producing fully/partly known; ★narrow load 8/16-bit signed vs unsigned (sign-bit set: 0x80/0x8000); UBFX lsb/width boundaries (lsb 0, width 1, lsb+width=32, lsb+width>32); unknown operand blocks fold; idempotence.

**const_prop / const_var_prop** — ★`x+0`, `x-0`, `x-x`, `x*0`, `x*1`, `x*2^k`, `x&0`, `x&-1`, `x|0`, `x|-1`, `x^x`, `x^0`; ★`INT_MIN + -1`, `INT_MAX + 1` wrap; ★shift by 0/31/32/neg; ★signed vs unsigned div/mod, `INT_MIN/-1`, div/mod by 0 (must bail, not crash); 64-bit (INT64) folds; immediate not encodable in ARM (large constants); multi-def / addr-taken / non-const guards; idempotence.

**copy_prop** — copy of copy (chain); ★`is_lval` on src / dest / use in each slot preserved; redefinition between def and use blocks prop; copy across a CALL; copy across a merge point; self-copy; width-mismatch not recorded; STORE-dest pointer rewrite; idempotence.

**cmp_field_fuse / cmp_expr_fold / cmp_const_offset_fold** — field width 1 and 31; signed vs unsigned compare; offset 0 / negative / overflow; base mismatch; `is_lval` base; CMP not adjacent to the def; ≥3 fields; non-NE/EQ conditions; idempotence.

**licm** — invariant in a simple loop (positive hoist) ★asserting the moved instr lands in the dominating preheader; nested loop (hoist to the right level); op that is *not* invariant (in-loop def) stays; ★side-effecting op (STORE/CALL/div-by-maybe-0) must NOT hoist; back-edge to index 0; multiple back-edges; no-preheader case; deref/aliasing load not hoisted; straight-line (no loop) → 0; no crash on malformed-ish CFG.

**jump_threading / eliminate_fallthrough** — chain length 1/2/N; ★cycle A→B→A must terminate (no infinite loop) and not corrupt; jump-to-self; target 0 / n-1 / out of range; conditional vs unconditional; fallthrough across NOPs; backward-edge guard; preserve real branch; idempotence.

**setif_or_tautology** — every condition code; tautology (always-true) vs contradiction (always-false); ★fold result value (#1 / #0) computed independently; mismatched CMP operands; signed vs unsigned; partial mask union (no fold); SETIF without preceding CMP; idempotence.

**dead_lea_store_elim** — store then load same / overlapping / disjoint byte ranges (★only disjoint/overwritten is dead); store width vs load width mismatch; address escape (LEA result stored/passed) bails; multiple stores to same slot (earlier dead); no-temp early-out; volatile/lval kept; idempotence.

**self_copy_elim / float_narrowing** (needs the get_tok_str table from §B) — ★real memcpy/memmove/`__aeabi_mem*` self-copy folds to NOP; non-matching name does not; ★f2d→…→d2f narrowing chain actually narrows (with the name table); partial/!4-instr chains decline; null callee.

---

## D. Execution
- **Fan out one agent per pass** (10–11 agents), each *appending* corner-case tests to its existing
  `test_opt_*.c` (distinct files → no write races). Same rules + the `verify`-then-`make ut` discipline.
- Do §B harness extensions **first** (one focused change set) so agents can use `utb_emit4` / flag
  helpers / the get_tok_str table. These touch shared files (`ir_build.h`, `stubs.c`) → do serially, not in the fan-out.
- After the fan-out: single `make ut` (expect ~589 → ~150+ more tests), then build+run the unit binary
  under ASAN once for the robustness dimension.
- Record every confirmed wrong result in `PASS_COVERAGE.md` *Findings* (do not fix).

## E. Sequencing
1. §B harness extensions (serial, ~0.5 day).
2. Per-pass corner-case fan-out (parallel, the bulk).
3. ASAN run of the unit binary over the new tests.
4. *Then* Track 3 (gcc diff) and Track 4 (IR metamorphic) from `plan_bug_hunting.md`.

The arithmetic semi-oracle tests (★) in const_prop/const_fold/known_bits/setif are the most likely to
surface real bugs in this phase; prioritize those within each agent.
