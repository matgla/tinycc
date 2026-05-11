# Fix plan: SL-FWD multi-pred merge alias bug

## Goal

Make this pattern correct at any `-finline-limit`:

```c
caller_local.field = X;
callee(&caller_local);   // inlined; callee conditionally writes through pointer
use(caller_local.field); // sees stale X instead of the new value
```

Concretely: 20090527-1.c passes at `-O2 -finline-limit=120`, suite stays green, then re-raise -O2 default threshold to 100 → test.txt main 64 → 44.

## Reference repro

Minimal C, no torture-suite dependency. Triggers the bug at `-O1 -finline-limit=80` (or any threshold ≥ 76) on unmodified baseline. Currently compiles `main` to unconditional `bl abort`.

```c
typedef struct { int pos; int status; } S;
extern void abort(void);

static void inner(S *flags) {
  if (flags->status == 1) flags->status = 0;
  switch (flags->status) {
    case 0: break;
    default: abort();
  }
}

int main(void) {
  S f;
  f.status = 1;
  inner(&f);
  return 0;
}
```

## Bisect results

Both of these individually fix the bug:

- `-fno-store-load-fwd` → 18 insns in `main`, correct behavior
- `-fno-dead-store-elim` → 13 insns, correct behavior

So the bug is the **interaction** between SL-FWD (`tcc_ir_opt_sl_forward` at `ir/opt.c:11553`) and dead-store elimination, surfaced when the inliner emits the pattern:

```
TEMP <-- Addr[StackLoc];
VAR <-- TEMP [STORE];
... TEMP' <-- VAR [ASSIGN]; deref(TEMP'+offset) ...
```

## Step 1 — Diagnostic infrastructure (prerequisite)

Without this, we're guessing. Cost: ~2 hours; pays for itself the moment we hit the next IR bug.

1. **Per-pass IR dump hook.** Add `tcc_state->dump_ir_passes` (comma-separated list or `all`). In `tccgen.c:25684-26154`, after every `tcc_ir_opt_*()` call between BEFORE and AFTER-LOOP-ROTATION, emit `=== AFTER <pass_name> ===` + IR dump if the pass name matches. Gate with `#ifdef CONFIG_TCC_DEBUG` so release builds aren't bloated.
2. **SL-FWD state tracer.** Inside `ir/opt.c:11553`, log every entry to `hash_table[]` at each significant event: STORE TRACK, LOAD FORWARD, BB RESET/RESTORE/KEEP, JUMPIF snapshot. One line per event, keyed by `(sym, offset, value, source_idx)`. Already partially exists via `LOG_SL_FWD`; extend to be exhaustive.
3. **Repro fixture.** Add `tests/ir_tests/test_sl_fwd_alias.c` (the minimal repro above) with `expect` file `PASS\n` and compile flags `-O2 -finline-limit=80`. **Currently fails** — that's the point.

## Step 2 — Pinpoint the corrupting pass

With (1) in place: dump IR after each pass from BEFORE to AFTER-LOOP-ROTATION, diff between adjacent dumps, find the pass that turns "correct IR with conditional store" into "broken IR where switch always picks default."

Hypotheses ranked by likelihood:

- **(H1) `tcc_ir_opt_sl_forward` at the multi-pred merge after the conditional store** (`ir/opt.c:12054-12153`). The snapshot/restore logic resets state at multi-pred targets but may carry forward the wrong entry on the post-merge path. Specifically: when the conditional `if (status == 1) status = 0` is partially folded across iterations of the SL-FWD loop, the entry for `&f.status` may survive the merge with the value `1` instead of being killed by the conditional store.
- **(H2) `tcc_ir_opt_dse` / `tcc_ir_opt_dead_var_store_elim`** (`tccgen.c:26119`, `tccgen.c:26130`). Bisect showed `-fno-dead-store-elim` alone fixes the bug. So dead-store-elim is removing the conditional store under a wrong "no live reads" determination — possibly because SL-FWD already forwarded all the reads (from the wrong store), leaving the *correct* store dead-looking.
- **(H3) Interaction**: SL-FWD-iter-1 forwards the conditional-branch CMP correctly, branch_fold eliminates the branch, then SL-FWD-iter-2 sees the now-unconditional path but its `pred_count` recomputation (`ir/opt.c:11610`) over-counts a phantom predecessor (NOP fallthrough from the eliminated branch), forcing a multi-pred reset that kills the kill-tracking for the conditional store.

H3 is the most plausible: it explains why the lea_map fix didn't help (lea_map was fine; the failure is in per-iteration `pred_count` interacting with NOPs left by the previous iteration's branch_fold). The comment at `tccgen.c:26031-26036` hints at exactly this: `compact_nops` was added to mitigate phantom-predecessor counting, but may be incomplete.

## Step 3 — Fix, narrowed to whichever H1/H2/H3 the diff confirms

### If H1/H3 (SL-FWD state at merges):

- **Option A (preferred):** Before each SL-FWD iteration, run `tcc_ir_opt_compact_nops` *and* recompute `pred_count` from scratch on the compacted IR — already mostly done, audit for completeness. If after this `pred_count` is still off, the iteration-2 pred_count is reading stale `is_jump_target` flags; force a flag rebuild.
- **Option B:** At merge points, *unconditionally* kill any tracked entry whose store happened on only one predecessor path. Conservative but provably correct. Cost: slightly less aggressive forwarding — quantify on the suite.

### If H2 (dead-store-elim):

- Audit the "candidate for dead-store-elim" list (`fwd_stores[]` at `ir/opt.c:11587-11595`). The candidate is added when SL-FWD forwards a load *from* that store. If we later kill that forward (because a subsequent iteration discovers a closer store), the candidate must be retracted. Currently it's a one-shot list; needs a "retract on overwrite" mechanism.

### If something unexpected:

The dump diff will tell us. Don't pre-commit to a fix.

## Step 4 — Validation

1. **Repro test:** the new `test_sl_fwd_alias.c` passes (`PASS` output, exit 0).
2. **GCC torture test:** `20090527-1-O2` passes at default and at `-finline-limit=120`.
3. **Full suite:** `make test -j16` → 12335 passed (no new regressions).
4. **Hand-crafted alias tests:** add 3-4 variants of the repro covering:
   - Store + cond store + load (this bug)
   - Store + unconditional store + load (must still forward correctly)
   - Store + call-that-might-write + load (must NOT forward — addrtaken)
   - Nested struct field stores at different offsets (must not alias)
5. **Codegen sanity:** spot-check a few `-O2` builds (the test.txt example + complex existing tests) — no unexpected size growth from a too-conservative fix.

## Step 5 — Then re-raise the threshold

Two-line change in `libtcc.c:2304-2306`: set `opt_inline_limit` floor to 100 at `-O2`. Re-run the suite. Re-measure test.txt — expect 44 instructions on main (vs current 64, GCC's 26).

## Risks & mitigation

- **Risk: fix is too conservative, regresses size on benign code.** Mitigation: benchmark on `tests/ir_tests` corpus before/after — if any test grows by >5% instructions, narrow the fix.
- **Risk: fix masks a different bug.** Mitigation: the hand-crafted alias-test set in Step 4 is specifically designed to be sensitive to both directions (false-positive forwarding *and* missed forwarding opportunities).
- **Risk: the bug is actually in the inliner's emission shape, not in SL-FWD.** Mitigation: Step 2's dump diff will show this — if the bug-introducing pass is in the inline-expansion path itself rather than SL-FWD, pivot to fixing the inliner.

## Estimated effort

| Step | Time |
|------|------|
| 1. Diagnostic infrastructure | ~2h |
| 2. Pinpoint pass + confirm hypothesis | ~1h |
| 3. Implement fix | ~2-4h |
| 4. Validate | ~1h |
| 5. Re-raise threshold + measure | ~30min |
| **Total** | **~6-8h** |

## When to abort

If Step 2 turns up that the bug is in a load-bearing alias-analysis invariant (not just a missed kill), and the fix touches more than ~50 lines of SL-FWD, switch to side-stepping it in the inliner: have the inliner emit LEA ops or a different parameter-binding shape so the bug isn't triggered. Smaller surface, even if it leaves the underlying bug unfixed.

## Context: what's already in place

- **Phase 0 stash** in `tcc.h` + `tccgen.c`: optimized IR of eligible `static` functions is retained past `gen_function()` (mapped on `TCCState->stashed_func_irs`) and flushed at `tccgen_finish`. No consumers yet; foundation for a future IR-level inliner that would side-step the token-replay inliner's parse-time constant gate.
- **Existing token-replay auto-inliner** in `tccgen.c:14872` and `tccgen.c:27844`: works for functions ≤60 tokens at `-O2`. Gated by `func_auto_inline` (always inline) or `func_eval_only_inline` (inline only when all call-site args are VT_CONST at parse time).
- **The bug in this plan blocks** raising the `-O2` default threshold beyond ~75 tokens.
