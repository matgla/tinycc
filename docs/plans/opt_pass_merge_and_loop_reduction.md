# Optimizer Pass Merging: Fewer Walks, Less .text

**Status**: Phases A, B1, D, F, G landed 2026-11-03/04 (eight commits on `opt/pass-merge-2026-11-03`, see §7); E closed after two measured variants (neither faster); B2/B3 open, expected below measurement noise; analysis measured on `perf/ra-time` 074081d1
**Created**: 2026-11-03
**Goal**: Merge the passes that re-derive one analysis in several walks, so a
function is traversed fewer times per compile and the optimizer's share of the
device compiler's `.text` shrinks, without changing emitted code except where a
phase says so.
**Builds on**: [`opt_pass_dedup_and_perf.md`](opt_pass_dedup_and_perf.md)
(compile-time plan, Phase 0-1 landed) and
[`opt_text_consolidation.md`](opt_text_consolidation.md) (`.text` plan; A, B
and C landed, D1-D4 open).  Both are referenced by item number below rather
than restated.
**Does not reopen**: retiring the flat tier (closed in
`plan_legacy_flat_ir_ssa_retire.md`; `duplication_analysis.md` verdicts the
flat/SSA pairs "not removable"), merging `run_gens` tables (first-match-wins
makes two tables inequivalent to one), or cutting SSA iterations (reruns on
identical IR were measured at ~6% of SSA time on `perf/pass-skip`).

---

## 1. Shape of the pipeline today

One function at `-O2` passes through, in order:

| Stage | Where | Passes | Fixpoint |
|---|---|---|---|
| switch_head, const_call_replace | `source/opt/function_pipeline.c` | 2 | - |
| propagation group | `engine/pipeline_table.c` | 29 entries | ≤10 rounds, no trigger |
| fusion / disp / chain / lea / bool | `function_pipeline.c` run_opt_pipeline | 11 | - |
| entry_store group | `pipeline_table.c` | 3 | ≤3 rounds |
| memory group | `pipeline_table.c` | 10 entries | ≤12 rounds, trigger sl_forward; `kb_cascade` 8 rounds and `const_cascade` 4 rounds nested |
| addrof folds, global_sl_fwd, hoist | `function_pipeline.c` | 5 | - |
| late_cleanup group | `pipeline_table.c` | 24 entries | ≤2 rounds, then up to 4 full reruns behind call_result |
| post pipeline | `function_pipeline.c` run_post_pipeline_passes | ~20 | - |
| flat loop transforms | `source/ir/regalloc_entry.c` | ~27 | each builds its own CFG + dominators; two may rerun the propagation group |
| SSA driver | `source/opt/ssa/engine/driver.c` | 34 per round | ≤5 rounds, bool_norm, 8-round tail, guard_collapse; duplicated as the `RUN_SSA` clone in regalloc_entry.c for the non-promotable path |
| post-SSA | `regalloc_entry.c` | ~15 | four CFG rebuilds |
| RA, post-RA, codegen | backend | - | three codegen walks |

Measured with the host cross (`TCC_PASS_TIMING=1`, 300 gcc-torture compile
files + tests2, 422 TUs, 805 functions, `-O2`):

| Metric | Value |
|---|---|
| Distinct timed pass names | 145 |
| Timed pass invocations per function | 174 (plus ~20 untimed straight-line passes and cascade internals) |
| `for` loops over an instruction index in `source/opt` | 668 |
| Optimizer share of compiler `.text` (x86 cross proportions) | ~50%; flat 60% of it, SSA 30%, engine/util 10% |
| Host optimizer time, flat tier | 450 ms (298 ms without `ssa:reroll`) |
| Host optimizer time, SSA tier | 219 ms (67 ms without `ssa:reroll`) |
| Passes that never fired on the corpus | 14% of optimizer time, 39K invocations |

Two caveats.  The ARM `.text` split is by proportion from the x86 cross's
unity objects (the device ELF is stripped); the 2026-08-07 ARM measurement
was 738 KB of 1,437 KB.  Host time under-weights memory-heavy passes: the
board model shows `ssa:decrement_to_carry` and `ra:loop_unroll` at tens of
seconds per unit with zero changes, where the host shows well under 1 ms.
Rank by the board model (`scripts/board_model.py`, `selfhost_profile.py`)
before committing to a compile-time item.

## 2. Measured clusters

The two families below are 42% of host optimizer time between them and
account for most of the re-walking.

### 2.1 Dead-store family (late_cleanup)

Thirteen passes: `dse`, `dead_local_slot`, `dead_addrvar`,
`dead_trail_addrvar`, `dead_var_store`, `dead_static_store`,
`dead_alloca_vreg`, `dead_lea_store`, `dead_temp_local`, `redundant_assign`,
`byte_store_merge`, `slot_const_store_fold`, the VLA trio
(`dead_vla_struct`, `alloca_load_fwd`, `zero_vla`).

| Fact | Value |
|---|---|
| Source | ~6.4K lines over `flat/memory/{dse,dead_local_slot,dead_lea_store,dead_temp_local,dead_static_store,var_store_elim,byte_store_merge,slot_const_store_fold}.c`, `flat/dce/{dead_addrvar_elim,dead_trailing_addrvar_store,dead_vla,zero_vla_elim}.c` |
| Runs per function | 1.43 (group rerun behind call_result + cfg_cleanup) |
| Host time | 143 ms = 21% of optimizer time |
| Productive | `dse` 11.8%; every other member ≤1%; `dead_addrvar`, `dead_alloca_vreg`, `dead_lea_store`, `dead_temp_local` 0 on the corpus |
| Private address resolvers | 7 pointer→(base, offset) walks in `flat/memory` alone, depth caps 4 / 32 / unbounded (`opt_pass_dedup_and_perf.md` 5.2) |

They all answer one question, "is this frame store read again before it is
overwritten or the function returns", with different shape matchers and
different address resolvers.  `perf/ra-time` 074081d1 already cut the
*second* late_cleanup run to `dse`, `redundant_assign`, `inplace_arith` with
identical `.text` over 4,113 test compiles, which is the evidence that the rest
of the family finds nothing the first run did not.

### 2.2 Flat constant dataflow (propagation + memory groups)

`const_prop_tmp`, `const_var_prop`, `value_tracking`, `known_bits`,
`const_agg_fold`, `symref_prop`, `global_init`, `global_sl_fwd`.

| Fact | Value |
|---|---|
| Source | ~5.7K lines in `flat/scalar/{const_prop_tmp,const_var_prop,value_tracking,known_bits,const_aggregate,symref_const_prop,global_init_prop}.c` |
| Host time | 140 ms = 21% |
| Productive | `const_prop_tmp` 5.1%, `const_var_prop` 4.2%, `value_tracking` 3.9%, `known_bits` 3.8%, `const_agg_fold` 0 |
| Spelled as | table entries, `const_cascade` (4 rounds), `kb_cascade` (8 rounds), `esp_cleanup`, `esp_value_tracking` |
| Shared logic duplicated | three integer const-fold binop tables (`const_prop_tmp.c`, `known_bits.c`, `value_tracking.c`) that disagree on shift ≥ width (dedup plan 4.6, a latent bug) |

### 2.3 Cleanup cascade spelled six times

`dce` + `eliminate_fallthrough` + `jump_threading` + `compact_nops` appears
as: the `kb_cascade` tail, `branch_cleanup_cascade`, the memory-group trigger
prologue, `esp_cleanup`, `ra_cfg_cleanup`, and the 8-round `ra:setif_fuse`
loop in `regalloc_entry.c`.  The same tables list `setif_fuse` three times
(plus the RA copy), `stack_bool` and `var_tmp_fwd` twice, `redundant_assign`
twice, `branch_cleanup` twice.  `CASCADE_END` exists only to patch the
generation counter for the mutations these hand-rolled copies do not report.

### 2.4 Two SSA drivers

`ssa/engine/driver.c` and the `RUN_SSA` clone at
`regalloc_entry.c` (promoted vs. non-promotable path): the same 34-entry
schedule with small deliberate differences (`var_forward` for
`var_to_param_forward`, no `tmp_block_const`/`cmp_eq_prop`/`dead_loop`/tail,
`guard_collapse` bare in the loop).  `opt_text_consolidation.md` D1 is still
open.

### 2.5 Loop cluster

Sixteen flat loop transforms before SSA each build CFG + dominators; four
(`loop_unroll`, `decrement_to_zero`, `iv_strength_reduction`,
`loop_const_sim`) carry verbatim ~60-line candidate drivers and ~40-line
`try_candidate` prologues (dedup plan 3.1-3.3).  Small on the host; the
expensive cluster on the board.

### 2.6 Per-function feature prescans

75 files under `flat/` privately walk the whole function for
`IJUMP`/`SETJMP`/`INLINE_ASM` before bailing; 19 build their own def-use
lookups (dedup plan 1.5, 2.1).  No `fn_features` bitmask exists yet.

### 2.7 Not a merge: two algorithmic outliers

`ssa:reroll` is 19% of host optimizer time, all of it in two files
(`tests2/101_cleanup.c` 122 ms, torture `20050423-1.c` 38 ms): a quadratic
candidate scan, not a steady cost.  `uninit_dom_ret` is still 2% at zero
productivity after the Phase 1 guard reordering; `const_agg_fold` 1.8% at zero.

## 3. Phases

Ordered by payoff ÷ risk.  Every phase lands separately.  "Byte-identical"
means `make test-golden-ir` shows no diff and the 76 self-host objects match
the cross (`scripts/tcc_selfhost.py`, `selfhost_profile.py identity`).

### Phase A — the dead-store family

**A1 — DONE (704194c5, byte-identical).** `ir_opt_addr_walk` in
`analysis/alias.c` (`opt_alias.h`) walks a TEMP's def chain through
LEA/ASSIGN, ADD #imm (SUB with `IR_AW_SUB`) and TEMP copies to the frame slot
or `&sym + addend` it addresses, under the calling pass's def model (a def
map, or a def count plus the latest def before the use).  byte_store_merge
and dead_local_slot use it; memmove_to_indexed_stores, struct_copy_roundtrip,
rmw_byte_clear and dead_lea_store stay apart, each with a comment naming its
delta.  Proof: 4,641 corpus objects (gcc-torture compile ×300, tests2,
ir_tests at -O1/-O2/-Os) hash identically, golden IR 27/27, make test CI=1
18047 passed.

Found on the way: `&?NNN` (an anonymous aggregate's StackLoc) carries a
frontend temporary's *negative* vreg before RA, so `irop_get_vreg(op) < 0` and
`== -1` are not the same test there.  byte_store_merge took such a slot,
dead_local_slot refused it; `IR_AW_TEMP_SLOT` keeps each as it was.  Letting
dead_local_slot take it is an A3 item.

**A2 — one points-to analysis (measured, not byte-identical).** Reading the
family shows that its real duplication is five private answers to "which
local's address may this TEMP or VAR hold", each with its own lattice and its
own idea of which use lets the local escape:

| Pass | Builder | Lattice | Copies followed | Conflict | A deref read is |
|---|---|---|---|---|---|
| dse (StackLoc part) | `prop_tmp`/`prop_var` fixpoint | origin addr-TEMP, -2 ambiguous | ASSIGN/STORE/LOAD/ADD/SUB, TEMP↔VAR | -2 | an escape |
| dead_addrvar | `DavFlow` | VAR, MANY, `other` | ASSIGN/STORE/LOAD of a vreg value; LEA into a TEMP only | MANY, both escape | an escape (never-read query) |
| dead_trailing_addrvar | `DtaPtrs` | VAR + `inexact` | ASSIGN/LOAD/STORE with a register dest; LEA into TEMP or VAR; POSTINC = inexact | first kept, both escape | a read event (`last_read`) |
| dead_lea_store | `TmpAddr` | single-def LEA offset | none | — | a `ReadEvent` at a position |
| dead_local_slot | tame slots via `dls_vreg_frame_off` | slot offsets | the A1 walk | — | poisons the slot |

Because the lattices differ and the same use means different things, no
shared builder reproduces all five byte for byte.  A2 therefore builds one
`frame_ptr_facts` analysis with the most precise lattice (exact target,
`inexact` flag, MANY, escapes on conflict, and per-position read events), and
each consumer keeps only its query.  Order, one pass per commit:

1. dead_trailing_addrvar + dead_addrvar (adjacent in late_cleanup, same
   question asked twice; dedup plan 4.11).
2. dse's StackLoc part (`prop_tmp`/`prop_var`, addr_tmp/addr_tmp_read).
3. dead_lea_store, then dead_local_slot.

Validation of every step: the corpus hash diff (list every changed object and
explain each as a store now proven dead, or one now kept), `make test CI=1`,
golden IR regenerated where the change is intended, the kernel battery, and
device == cross on the phase's final tip.  The per-pass bails
(IJUMP/SETJMP/asm/VLA/chain/SWITCH_TABLE/backward jumps) move into the shared
facts as Phase F's bitmask.

**A3 — fold the shapes.** With one facts table, `dead_addrvar`,
`dead_trailing_addrvar`, `dead_lea_store`, `dead_alloca_vreg` and
`dead_temp_local` become shape rules over the same read events; dse's VAR
part and `dead_var_store` become one never-read query.  Expected: 10+
whole-function walks per round → 2-3; ~3-4K lines → ~1.5K.

**A4 — DONE first, by measurement (byte-identical).**  Before merging
anything, every flat pass was disabled in turn (`TCC_DISABLE_PASS`, an exact
name match on the table entry) and the compiler's output hashed on three
corpora: 4,641 test objects (the first 400 gcc-torture compile files, tests2
and ir_tests at -O1/-O2/-Os), tinycc's own 77 self-host units from the frozen
`tcc_selfhost` payload, and the cached Zig-CBE kernel (`.cache/kernel_tcc`)
at -O1/-O2/-Os.  A run takes about 10 s per pass on the host, so this is the
cheap gate for every later phase (§6 has the table).

Eleven table entries change no object on any corpus, and went:

| Removed | What it was | Lines |
|---|---|---|
| `dead_trail_addrvar` | pass, `flat/dce/dead_trailing_addrvar_store.c` | 373 |
| `inf_loop_simpl` | pass, `flat/dce/infinite_loop_simplify.c` | 469 |
| `uninit_dom_ret` | function in `flat/dce/uninit_ub.c` (`uninit_ub` stays, 4 objects) | 163 |
| `dead_alloca_vreg` | function in `flat/dce/dead_vla.c` | 274 |
| `cmp_xor_cancel` | standalone entry; the fold still runs inside `cmp_expr_fold` | 19 |
| `branch_cleanup` ×2 | the 8-round elim_fallthru+dce cascade in late_cleanup (and its golden case) | 20 |
| `var_tmp_fwd` ×2, `var_to_tmp`, `self_arith`, `elim_fallthru` | duplicate entries in the propagation and memory groups; the passes still run from their other spellings | 5 |

That is 12 whole-function walks per late_cleanup round and 5 per
propagation/memory round fewer, about 1,900 lines, and 5,015 objects (the
three corpora, 4c7cb5d9 vs. the trimmed tree) hash identically.  The unit
tests of the removed functions went with them; the ir_tests they once
motivated (`bug_dead_trail_*`, `494_dead_addrvar_store_indexed`,
`407_cmp_xor_cancel`) stay as correctness tests and still pass.

Kept although the test corpora never showed it: `dead_pre_inf` (the kernel
at -O2/-Os grows 20 bytes without it) and `uninit_ub` (4 test objects).
Lesson: the test corpora alone are not enough to call a pass dead; the kernel
C is the third corpus every removal needs.

Not touched: the RA knobs that also measured zero (`ra:jumpif_to_next`,
`ra:setif_chain`) are outside this plan.

**A4b — the same measurement over the SSA, RA, inliner and codegen gates
(203 names, byte-identical).**  Nine knobs change no object on any corpus.
Removed: `ssa:var_const_fold` (cprop.c, and its golden case),
`ssa:var_imm_prop` (whole pass), `ssa:setif_or_taut` (its sibling
`ssa:setif_mask_fold` stays), `addrof_var_fwd` (whole flat pass),
`ra:setif_chain` (`tcc_ir_setif_test_chain_fold`), the identity-BFI fold of
`ra:known_ext`, licm's pure-call hoist (`tcc_ir_hoist_pure_calls` and the
five helpers only it used, docs/bugs.md #7), and load_cse's `var_fwd` mode
(the VAR-slot forwarding branch and its five `vslot_*` helpers).  Kept:
`ra:ret_share_exit`, whose knob drops a soundness guard rather than a
transform.  About 1,400 lines; 5,015 objects identical; the unit tests of
the removed code went with it.

The knobs that are timers only (`ssa:decrement_to_carry`, `ra:loop_unroll`,
the `cg:`/`ra2:` names) cannot be measured this way; §1's board-model
caveat about them stands and needs a code-level switch first.

**Board model after A1 + A4 + F + A4b (2026-11-04, `scripts/board_model.py`
images `pm_base` = 4c7cb5d9, `pm_new` = 4cdf1443, eight units):** 245.2 s →
243.3 s, −0.8 %, every unit's object identical.  As expected: what went was
cheap walks that found nothing; the board's time is in the passes that do
work (§1 caveat).  The size win is the real one: about 5,000 lines of pass
code and some 30 walks per function fewer.

**A2 step 1 — DONE: one points-to table for dse's StackLoc part and
dead_addrvar (byte-identical after all).**  `analysis/frame_ptr.c`
(`ir_opt_frame_ptr`, opt_alias.h) builds, once per flat-engine generation,
the facts both passes used to derive privately: objects are the VARs a LEA
takes the address of and the anonymous StackLocs an instruction materialises
into a TEMP (one object per such TEMP position, dse's "origin"); nodes are
TEMP and VAR vregs; one fixpoint follows an address through register copies
(ASSIGN, LOAD, STORE into a tracked node) and pointer arithmetic (ADD, SUB of
the pointer with anything); a node with another kind of definition is
inexact, one that may hold two addresses is MANY and flags both objects;
every other use of a node that may hold an address -- a deref read, a value
stored or passed on, an index, a volatile or advancing store base -- reads the
object (a MANY node read reads every object).  dse maps `addr_tmp` /
`addr_tmp_read` onto the anonymous objects and keeps its slot-range liveness
and kill chain; dead_addrvar is a 60-line query (VAR has a LEA into a TEMP,
no LEA into a VAR, not read by name or through a pointer, no inexact or MANY
pointer) and deletes the LEAs, the stores through its pointers, the writes
by name and the copies and arithmetic that carried the address.  The two
private fixpoints and three walks per round became one shared computation.

Measured: 5,015 objects identical to 4c7cb5d9.  The semantic unification
(LOAD counted as a copy, ADD/SUB-derived pointers followed for dead_addrvar,
STORE_POSTINC and SLOT3 uses counted as escapes) changed no object on any
corpus.

Caught on the way, by the rule that every differing object is explained
before anything lands: the first version took a copy of &local into a PARAM
vreg (`P1 <-- T [STORE]`, decl_designator's `p = &s` before a call) for a
tracked copy, because the copy helper only looked at the source; the
address left the facts unseen and dse deleted the local's initialiser store
under the call.  Three tinycc units shrank by 120 bytes and looked like a
win.  A copy into anything the facts do not follow is an escape now;
`bug_frame_ptr_addr_into_param.c` (fails at -O1/-O2/-Os with the bug) and two
unit tests pin it.  The test corpora never contained the shape: the
self-host units are a required corpus, and "smaller" is not evidence.

**A2 step 2 — DONE: dead_lea_store's single-def LEA table comes from the
facts (measured, 5 test objects smaller).**  The facts gained a per-node
definition count and the materialising instruction per object; the pass's
Pass 1 walk is a lookup (single definition, exact, a plain ASSIGN/LEA of a
bare StackLoc).  One difference in what counts as a definition changed
output: the old table counted a STORE_INDEXED's base register as a
definition of that TEMP (`T9 <-- T13 STORE_INDEXED #8` stores through T9, it
does not write T9), so any address TEMP used as an indexed-store base lost
its offset and the pass could not range-check the slot.  The facts, like
dse, do not.  Five test objects (`bug_dce_overwrite_mac_accum_read` -O2/-Os,
`bug_regpair_static_aggregate_return` -O1/-O2/-Os) lose 4-14 bytes each:
zero-initialisers of a struct overwritten field by field before the first
read, and a bitfield read-modify-write store overwritten by the full-word
store that follows.  Both programs run correctly at every level; the
self-host units and the kernel are unchanged.

**dead_local_slot stays on the A1 walk.**  Its resolver is position-sensitive
(the latest definition before the use) and offset-precise (ADD/SUB deltas,
32 hops); the facts are flow-insensitive and object-level.  Folding it in
would mean an offset lattice and a loss of precision wherever a pointer TEMP
is redefined, for a pass worth 41 objects.  Not worth it; A2 ends here.

### Phase B — one forward dataflow for the flat constant family

**B1 — DONE: one integer constant-fold table (measured: a latent miscompile
fixed).**  `ir_opt_const_binop` in `util/const_eval.c` replaces four private
tables: `const_prop_tmp`, `known_bits`, `value_tracking` and the SSA tier's
`sccp_eval_binary` (which `tmp_block_const` also calls).  Each caller keeps
its own operand model (which width it folds at, how it narrows or masks the
result, which ops it admits); only the arithmetic and the "is the fold
defined" rule are shared.  The rule for shifts is const_prop_tmp's and
known_bits': a count outside [0, width) is the program's UB and the ARM shift
that would run uses the count's low byte, so the fold is refused and -O2
behaves as -O0.  value_tracking and sccp used to mask the count the x86 way
and folded `1u << 40` to 256, `0x80000000u >> 33` to 0x40000000.
`bug_const_fold_shift_count_out_of_range.c` (fails on the baseline at
-O1/-O2/-Os) pins it; `791_shift_count_out_of_range_not_folded` now stores
the ARM values (0, 0) in its never-run branch where it stored 256 and 1.
Everything else on the three corpora is unchanged.

2. **B2.** Fuse `known_bits` + `value_tracking` (dedup 5.7, re-ranked as the
   largest remaining compile-time item in that plan's §8): one lattice
   carrying known bits and tracked values, one forward walk.
3. **B3.** `const_prop_tmp` + `const_var_prop` become the propagation step of
   the same walk; `const_cascade` and `esp_cleanup` lose their private
   spellings.  `const_agg_fold` gets a precondition (any aggregate store in
   the function) or goes.

### Phase C — one cleanup helper — reassessed, deferred

Read after A4 removed the two `branch_cleanup` cascades: the five spellings
left (regalloc_entry's jump-threading cascade and the two `ra:setif_fuse`
tails, the memory group's trigger prologue, the `kb_cascade` tail) differ in
order (dce before or after elim_fallthru), in what they count (`silent` vs
reported), in what else they interleave (bool_diamond, jumpif_invert,
orphan_cmp, jump_threading) and in whether compaction is conditional.  A
helper that reproduces all five byte for byte needs more flags than the
sequences it replaces, and the value table shows none of the spellings is
free to drop.  Nothing to merge here without changing output; revisit only
if a later phase makes two of them identical.

### Phase D — one SSA driver — DONE (byte-identical)

`ssa/engine/driver.c` now holds three `const SsaPass[]` schedules (slot,
gate, name, function): the main loop, the convergence tail and the fallback
for a body with nothing promotable.  One `ssa_run_one` applies the gate, the
timer, the generation-skip and the `-dump-ir-passes` hook; `ssa_run_list`
walks a schedule.  `tcc_ir_ssa_opt_run_fallback` replaced the `RUN_SSA` clone
in `regalloc_entry.c`, whose orders stay deliberately different
(`ssa:var_forward` for `ssa:var_to_param_forward`, no loop passes,
guard_collapse bare in the loop, bool_norm followed by dce).  The fallback
bumps the generation after every guard_collapse round, so it skips nothing
across rounds, exactly as before.  Gate: 5,015 objects identical, golden
25/25, unit 0 failed.

### Phase E — one CFG for the loop cluster — CLOSED, measured twice (2026-11-04)

The redundancy is real: over the 77 self-host units, 73% of the 57,669 CFG
builds and 63% of the 38,465 loop detections see control flow identical to
the previous build (an exact key -- per instruction the opcode, a jump's
target, an asm goto's targets, a switch table's entries, a call's
noreturn-ness -- compared build to build).  Two ways of exploiting it were
built, both byte-identical and green on every suite, both measured on the
board model's eight units against the tip without them (239.7 s):

1. A keyed cache behind `tcc_ir_cfg_build` (with dominators) and
   `tcc_ir_detect_loops`, callers receiving deep copies: **246.4 s, +2.8%**,
   data reads up, peak heap +100..450 KB.  The copies write about what a
   build writes.
2. A reference-counted shared master CFG, no copies, dominators / RPO /
   frontiers computed once and idempotent: **239.3 s, -0.2%**, mixed per
   unit, peak heap +0..200 KB.  The key pass reads what the builder's leader
   scan reads, so the saved edge and dominator work is of the same order.

So the builds are not where the loop cluster's 19 s go; the passes' own
walks are.  The post-SSA region's twelve rebuilds are already gated on a
pass having reported a change.  Closed: neither variant lands.  What would
still help is not caching but fewer passes building at all -- e.g. the
loop drivers (`loop_cand.c`) sharing one CFG across their candidates, which
is per-pass work with per-pass measurement, outside this plan's budget.

### Phase F — `fn_features` bitmask — DONE (byte-identical)

`ir_opt_fn_features(ir)` (opt_utils.h, block_scan.c) returns sixteen
opcode-presence bits (IJUMP, SETJMP/LONGJMP and their NL_ forms, INLINE_ASM,
ASM_INPUT/OUTPUT, SET_CHAIN, INIT_CHAIN_SLOT, SWITCH_TABLE, SWITCH_LOAD,
VLA_ALLOC, any call, any JUMP/JUMPIF, any IROP_A_MAY_BRANCH op, and a jump
target after the entry).  The cache lives on `TCCIRState` and is keyed on the
flat engine's generation counter: `pipeline_call` publishes
`ctx->generation` as `ir->opt_generation` around a *plain* pass and clears it
after, so plain passes of one round share one scan, while cascades (whose
sub-passes mutate before the counter moves) and every caller outside the
engine scan afresh.  `tcc_ir_opt_ctx_init/free` invalidate, so a nested
context can never leave a stale value behind.  The soundness argument is the
same one the engine's clean-pass skip already rests on and golden IR gates.

Nineteen private prescans became bit tests: abort_tail_merge, dead_pre_inf,
dce, dead_vla_struct, uninit_ub's helper, stack_param_promote,
byte_store_merge, dead_local_slot, deref_operand_cse, global_base_share,
global_sl_fwd, dead_var_store, addrof_const_fold ×2, const_var_prop,
dead_lea_store (whose call loop now also skips when there is no call),
zero_vla, stack_addr_fold, and setif_fuse's zero-return fold.  Not converted:
scans inside helpers that run after the pass has started mutating (licm's
flag check, loop_rotate's target refresh), dse's chain check (mid-pass), and
loops that compute more than presence (const_agg_fold's jump-range check,
byte_store_merge's per-candidate isolation scan).  Unit tests cover the bits
and the cache keying.  Gate: 5,015 objects identical, unit 0 failed, golden
26/26, make test CI=1.

### Phase G — the two outliers — DONE (byte-identical)

`ssa:reroll`: a run of period P also opcode-matches at 2P, 3P, ... up to the
32-instruction cap, and each such candidate re-walked the whole run with the
full rename-aware matcher.  tests2/101_cleanup's 262K-instruction function
walked 2.1M instructions (178K block matches) for one re-roll: 267 ms, 19%
of the file's compile.  An opcode-only repeat count (one byte compare per
instruction) now bounds each candidate first; a candidate whose bound cannot
beat the best score so far is skipped.  The bound is exact (a full match
implies an opcode match), so decisions are unchanged: 44 ms, 3%.
gcc-torture 20050423-1 was already handled by the copy-body rejection.

`const_agg_fold`: it rewrites only dadd/dsub calls, so a body without a call
(`IR_FN_CALL` clear) returns before its lattice walk.  `uninit_ub`'s guards
were already reordered by the dedup plan's Phase 1; left alone (4 objects).

## 4. Gates

| Gate | Command | Pass criterion |
|---|---|---|
| Unity manifest | `python3 scripts/gen_unity.py --check` | exit 0 |
| Host suite, CI configure | `./configure --enable-cross --enable-O2 --debug --enable-asan && make test CI=1 -j$(nproc)` | 0 failed |
| Float ABI | `make test-fp FLOAT_ABI=softfp` / `hard` | 0 failed |
| Golden IR | `make test-golden-ir` | byte-identical unless the phase says otherwise |
| Device = cross | `scripts/tcc_selfhost.py --qemu` or board; `selfhost_profile.py identity` | 76/76 objects identical |
| Kernel battery | `scripts/kernel_compare.py` against the stored baseline | rc + hashes equal; count deltas explained |
| Board time | `scripts/board_model.py` eight-unit model | not slower; the phases that promise time must show it here |

Self-host and fuzz run once, on the final tip of a phase, never per commit.

## 5. Integration baseline (2026-11-03)

The November compile-time work this plan measures against is combined on:

- tinycc `perf/combined-2026-11-03` = `perf/ra-time` 074081d1 (the linear
  stack `perf/include-dir-listing` → `perf/packed-copy-inline` →
  `perf/sram-ir-arena` → `perf/pass-time` → `perf/pass-skip` →
  `perf/ra-time`, 28 commits over `mob` v0.2.2 822f55e2) plus what the CI
  configure (`--enable-cross --enable-O2 --debug --enable-asan`) needed:
  - 3dcdb4e5 `tests/frontend/test_include_search.py`: the open() logging shim
    in LD_PRELOAD refuses to start under an ASan cross
    (`verify_asan_link_order=0`).
  - ac234fe3 `source/unity/groups.txt`: `hot_arena.c` includes no `tcc.h`, so
    it has no USING_GLOBALS mode and cannot share `memory_s1`; regenerating
    also regrouped the sources the stack had added without rerunning
    `gen_unity.py`.
  - 492ee221 `source/ir/tccir_operand.c`: since the 8-byte layout the cross's
    spare flag bits (`reserved2`, where `is_static`/`is_sym` lived) were
    stack garbage; `dup_diamond`'s byte compare of the flag byte then kept
    the second capacity guard of `bug_hash_map_capacity_guard` depending on
    the include path (bisected to 1df69da5; `test_hash_map_capacity_guard_once`
    red → green).  Lesson for every phase above: an operand must read alike in
    both layouts, so a constructor zeroes every bit it does not name, and a
    pass compares operands through the accessors, not `memcmp`.
- yasos `perf/combined-2026-11-03` = `zig-on-yasos` + `perf/toybox-sram`
  6867ab626 (merges f5965960, cd74a1d7; bump d17556c7) with libs/tinycc
  492ee221, libs/libc `perf/malloc-map-flags` 2343d0d1, apps/toybox
  `perf/low-sram` daf4e943, apps/yasvi `perf/stack-profile` 716f0c18,
  libs/lwext4 `perf/low-stack` 0a485d89.  Needs zig 0.18.0-dev.253 (the
  older zvm master silently drops the yasld-prelink loader precache from the
  rootfs).

Board self-host at tinycc 492ee221 + yasos 37e231ed5 (2026-11-03 20:49):
PASS, 1,159 s for compile + link (1,163 s at 074081d1; 30.2 min before the
stack), 77/77 objects identical to the cross, link identical.  **1,159 s is
the baseline every phase above is measured against.**  An earlier yasos tip
(79c9ccc66) passed every QEMU gate and still hard-faulted at the first mount
on the board (6867ab626 read an unset mutex owner), so a board boot stays a
required gate for kernel-side changes.  Open at the time of writing: stage 3 fixpoint, device tcc suite and fuzz not run;
`bug_u64_zero_high_shift` flakes under the full IR run (passes alone);
kernel `.text` grew 424 → 475 KB between cfbb6f5d and 074081d1 with flat
instruction counts (cause not investigated, c89dda22's `static inline`
estimate is the guess); nothing pushed.

## 6. Pass value table (2026-11-04)

Each flat pass disabled alone; objects that differ from the baseline and the
summed `.text` delta, over the three corpora of A4 (4,641 test objects, 77
self-host units, kernel ×3).  "Zero" means no object changed anywhere.  The
table is what a removal or a merge is argued from; redo it after every phase
with the scratch harness (`abcmp.py CROSS_A CROSS_B --env-b TCC_DISABLE_PASS=x`
is enough, ~30 s).

Zero on all corpora: `branch_cleanup`, `cmp_xor_cancel`, `dead_alloca_vreg`,
`dead_trail_addrvar`, `elim_fallthru` (memory-group entry), `inf_loop_simpl`,
`self_arith`, `uninit_dom_ret`, `var_tmp_fwd`, `var_to_tmp` -- removed in A4;
`ra:jumpif_to_next`, `ra:setif_chain` -- RA, left alone.

Zero on the test corpora, not on the kernel: `dead_pre_inf` (+20 B at -O2/-Os).

Small, kept (test objects differing / text delta, kernel equal unless said):
`ssa:rmw_byte_clear` 3 / +24, `const_agg_fold` 3 / +152, `switch_collapse`
3 / +236, `cmp_field_fuse` 3 / +88, `uninit_ub` 4 / +40, `escape_copy_call`
5 / +136, `ra:branch_thread_post_frame` 5 / +4, `shl32_or_chain` 6 / +24,
`zero_vla` 6 / +72, `pack64_tautology` 6 / +88, `esp_cleanup` 6 / +156,
`ssa:memmove_global_fwd` 7 / +148, `cmp_expr_fold` 8 / +38,
`ssa:unroll_cascade` 8 / +1340, `ra:exit_sink` 8 / +16, `dead_addrvar` 8 /
+18, `pack64_from_stack_stores` 9 / -68, `esp_value_tracking` 9 / +140,
`bf_insert_extract` 10 / +264, `dead_temp_local` 11 / +56, `self_copy_elim`
12 / +288, `ssa_string_fold` 12 / +565, `dead_vla_struct` 15 / +454,
`dead_static_store` 16 / +410 (kernel +8), `string_calls` 17 / +0,
`shift64_extract_ubfx` 18 / +190, `alloca_load_fwd` 18 / +330, `stack_bool`
18 / +374, `dead_var_store` 19 / +146 (kernel +20/+8), `dead_lea_store` 19 /
+618, `memmove_to_indexed_stores` 21 / -38, `ssa:slfwd_post_unroll` 21 /
+1364, `unreachable_fold` 24 / -152.

The dead-store family after A4: `dse` 472 / +6692 (kernel +2008/+1792/+1620),
`slot_const_store_fold` 72 / +816 (kernel +1896/+1860/+1004),
`redundant_assign` 64 / +654 (kernel ~+110), `byte_store_merge` 55 / +2088
(kernel +16..+64), `dead_local_slot` 41 / +1306 (kernel 0), then the small
members above.  Everything else in the family is worth under 20 objects; the
merge value (A2/A3) is in walks and lines, not in output.

Largest by objects changed: `frame_relayout` 1722, `sl_forward` 845, `select`
752, `inplace_arith` 709, `ssa:loop_addr_hoist` 690, `ssa:licm` 667, `sra`
516, `ssa:local_addr_cse` 474, `dse` 472, `const_local_table` 465,
`known_bits` 463.  Negative text deltas when disabled (the pass grows code,
by design or not): `ra:spill_split` -52702, `ra:loop_split` -49772,
`ssa:licm` -7190, `ssa:loop_addr_hoist` -2432, `ssa:stackoff_indexed_base_cse`
-2052, `global_base_share` -872, `sdiv_pow2` -612, `ra:live_through` -346,
`ra:load_sink` -266, `ssa:os_const_share` -210, `unreachable_fold` -152,
`pack64_from_stack_stores` -68, `memmove_to_indexed_stores` -38.

## 7. Landed (2026-11-04)

| Commit | Phase | What | Output |
|---|---|---|---|
| 704194c5 | A1 | one address walk for byte_store_merge and dead_local_slot | identical |
| a9414fba | A4 | 11 flat table entries that change no object removed | identical |
| 14272853 | F | `ir_opt_fn_features`, 19 prescans | identical |
| 4cdf1443 | A4b | 9 SSA/RA/inliner knobs that change no object removed | identical |
| bf52a070 | D | table-driven SSA driver, RUN_SSA clone gone | identical |
| f68da257 | A2.1 | `ir_opt_frame_ptr` shared by dse and dead_addrvar | identical (a miscompile caught before landing) |
| b1bc566d | A2.2 | dead_lea_store's LEA table from the facts | 5 test objects smaller, explained |
| 2258081b | B1 | one integer const-fold table for four folders | shift-count miscompile fixed, 2 test files |
| aa7d6056 | G | reroll candidate bound, const_agg_fold precondition | identical |

Compiler sources: about 5,000 lines fewer; three fewer fixpoints and some 40
fewer whole-function walks per function; the host proxy over the self-host
units -8%; the board model over all eight commits -2.2% (245.2 → 239.7 s on
the eight units, objects identical; the first four alone gave -0.8%, the
removed passes being cheap).  Gates on every commit: 5,015 objects over three corpora,
golden IR, unit suites, `make test CI=1`.  Still due once on the final tip:
stage-2 identity and stage-3 fixpoint under QEMU, the device tcc suite, fuzz,
and a board run before integration.
