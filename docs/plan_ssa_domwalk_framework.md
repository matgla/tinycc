# Plan: a shared dominator-tree scoped-walk engine for SSA passes

Status: **v1 landed; cmp_eq + gvn migrated** (2026-07-11). Engine
`opt_ssa_domwalk` + isolated UT shipped; both undo-log passes now ride it at zero
`make test` delta. Only the load_cse question remains (see below).

- Engine: `source/opt/framework/opt_ssa_domwalk.h` (header-only `static inline`,
  matching the `opt_dsl_phi.h` runner precedent — no new `.c`/Makefile SRC).
- Engine UT: `tests/unit/arm/armv8m/test_opt_ssa_domwalk.c` (9 tests, wired into
  UT11): empty/NULL cfg, single, linear chain, sibling scope isolation, tree
  coverage, change-count summing, NULL hooks, 3000-deep chain.
- Pilot: `source/opt/ssa/cfg/cmp_eq.c` — `process_block`/`CmpEqWork` deleted;
  fact stack encapsulated in a `CmpEqState` threaded via `state` (reentrant, no
  file-static globals). Behavior held by its 21-case suite.
- Second: `ir/opt/ssa_opt_gvn.c` — `gvn_process_block`/`GVNWork` deleted; the
  hash table rides `state` (`GVNState`), `mark`/`reset` wrap the file-static
  `undo_count`/`gvn_scope_pop_to`, `enter` NULL, `visit` = the value-numbering
  loop (block-local `lcache` stays a visit local). Undo stack / entry pool /
  param-mutated bitmap left file-static (reset per call). Behavior held by
  `test_ssa_opt_gvn`.
- Gate passed both times: `make cross` clean, UT11 317 green, `make test` 13529
  (zero delta).

Author-intent: decide whether to extract the dominator-walk skeleton that three
SSA passes currently hand-roll, and if so in what shape and order.

## Motivation: the engine is duplicated

Three passes independently reimplement a stack-overflow-safe dominator-tree walk
that carries per-subtree scoped state. Each copy also carries the *same*
hard-won comment about native recursion overflowing the 32 KB target process
stack on deeply branch-nested functions — i.e. each pass hit that bug and fixed
it by hand:

| Pass | File | Walk fn | Scope model |
|------|------|---------|-------------|
| cmp_eq | `source/opt/ssa/cfg/cmp_eq.c` | `process_block` (~L213) | undo-log watermark (`fact_count`) |
| gvn | `ir/opt/ssa_opt_gvn.c` | `gvn_process_block` (L565) | undo-log watermark (`undo_count`, `gvn_scope_pop_to`) |
| load_cse | `ir/opt/ssa_opt_load_cse.c` | `gload_process_block` (L797) | **heap snapshot cloned per branch** |

The duplicated skeleton is:

```
Work{kind,value} heap worklist; sp/cap; grow by *2
push root (kind==0)
while sp>0:
  pop; if kind==1: restore_scope(value); continue
  wm = current_scope_mark()
  (enter: derive/seed scope for this block)
  (visit: rewrite instructions in [start_idx,end_idx))
  push POP-marker (kind==1, value=wm)     # runs AFTER the whole subtree
  push each dom child (kind==0)
```

`cmp_eq` and `gvn` are near-identical: the POP marker restores a **monotonic
global undo counter** (`fact_count` / `undo_count`). This is a clean rule-of-two
for the *exact* engine.

## The crux: two different scope models

`load_cse` avoids the same overflow but does **not** share the engine:

- **Undo-log (cmp_eq, gvn):** one global table + an append-only undo stack; a
  child subtree appends, and `pop_to(watermark)` truncates on the way out.
  Restore is O(added). One live table.
- **Snapshot-per-branch (load_cse):** each pending work item OWNS a
  heap-allocated `GLoadState` copy (`gload_process_block` L813–821); a child
  branch gets its own clone, and there is no undo — divergent branches simply
  hold divergent copies. It also has a single-child fast path (inner `for(;;)`
  at L828) that iterates a linear dom chain without cloning, and a per-block
  `preds[pi] != idom` conservative reset (L842–854).

These are genuinely different contracts. Forcing all three onto one engine means
the engine must support both an undo-log and a clone/free lifecycle, which
erases most of the simplification. **Recommendation: target the undo-log family
first; treat snapshot-walk as a separate (possible) v2.**

## Proposed API — v1 (undo-log walk)

New header `source/opt/ssa/include/opt/ssa/domwalk.h`, engine in
`source/opt/ssa/cfg/domwalk.c` (rides in `SSA_OPT_SRC`). Analogous to how
`opt_dsl_run_phi_rules` owns the fixed-point block iteration for phi rules.

```c
typedef struct {
  void *state;                                  /* pass-owned scoped table    */
  int  (*mark)(void *state);                    /* return current watermark   */
  void (*reset)(void *state, int watermark);    /* restore to watermark       */
  int  (*enter)(IRSSAOptCtx *ctx, int block, void *state);   /* seed edge facts; #changes */
  int  (*visit)(IRSSAOptCtx *ctx, int block, void *state);   /* rewrite; #changes         */
} OptSSADomWalk;

/* DFS from block 0 over cfg dom_children; owns the heap worklist, the POP-marker
 * scheduling, and the watermark lifecycle. Returns total changes. */
int opt_ssa_domwalk(IRSSAOptCtx *ctx, const OptSSADomWalk *w);
```

The engine owns exactly what is triplicated: worklist growth, the
`kind==1` POP-marker that runs after the subtree, and `mark`/`reset` bracketing.
It does NOT own the `preds[0]==idom` edge guard or fact derivation — those are
pass semantics and live in `enter`.

### Hook mapping

**cmp_eq** (state = `CmpFact` stack):
- `mark` → `return fact_count;`
- `reset` → `fact_count = watermark;`
- `enter` → the `num_preds==1 && preds[0]==idom` guard + `try_push_edge_fact`
- `visit` → the CMP+JUMPIF fold loop (`try_fold_cmp`)

**gvn** (state = `GVNEntry**` table + undo stack):
- `mark` → `return undo_count;`
- `reset` → `gvn_scope_pop_to(watermark);`
- `enter` → empty (gvn seeds during visit, not from the edge)
- `visit` → the per-instruction LEA/SETIF/pair processing (which calls
  `gvn_scope_push`); note the block-local `lcache` stays a `visit` local

Both collapse the ~40-line `*_process_block` DFS to a small struct + two tiny
callbacks. The engine is unit-tested once (the 21-case `test_ssa_opt_cmp_eq`
suite is the ready-made regression net for the pilot; gvn has
`test_ssa_opt_gvn`).

## load_cse — why it stays out of v1

`gload_process_block` needs clone/free per branch, the single-child fast path,
and per-block reachability-aware reset. Options, in order of preference:

1. **Leave as-is.** Its snapshot model was chosen deliberately; the overflow fix
   already landed. No duplication removed, but no risk taken.
2. **v2 snapshot-walk engine** — a second `opt_ssa_domwalk_snapshot` with
   `clone(state)->state` / `free(state)` hooks and the single-child chaining
   built in. Only worth it if a *fourth* pass wants copy-on-branch scoping.
3. Convert load_cse to the undo-log model — **not recommended**; its state is
   large and multi-array, and undo-logging every field is more error-prone than
   the current snapshot.

## Migration order & validation gate

1. ~~Land `opt_ssa_domwalk` (engine + header) with a focused UT.~~ **Done.**
2. ~~Migrate **cmp_eq** first (smallest, best-covered).~~ **Done** — zero delta.
3. ~~Migrate **gvn**.~~ **Done** — zero delta. The block-local `lcache` stayed a
   `visit` local; the `gvn_try_*` return counts sum naturally through `visit`.
4. Revisit load_cse only if a snapshot-walk consumer appears. **Still open.**

Per repo policy the gate is `make test` + UT + a clean `-Werror` build; no fuzzer
/ `diff_olevels` runs.

## Non-goals

- **Not a declarative DSL.** Unlike `OPT_GEN_SSA` (PATTERN/GUARD/REWRITE), the
  per-block work here is semantic (read a terminator's EQ/NE token, symmetric
  fact lookup, store-kill). This is a callback engine, like the phi runner — not
  a match/rewrite surface.
- Not a change to pass *behavior*: every migration is byte-faithful, gated at
  zero `make test` delta.
```
