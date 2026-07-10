# Plan: Retire flat-IR scalar passes in favor of their SSA analogs

**Status:** in progress · **Created:** 2026-07-07 · **Updated:** 2026-07-10 · **Branch:** `legacyOptRemoval`

Follow-on to [`plan_legacy_loop_ssa_replacement.md`](plan_legacy_loop_ssa_replacement.md)
(loop passes — done). Scope here: pre-RA flat-IR **scalar** passes in the
`tccgen.c` tail that duplicate an SSA pass already running at `-O1+` via
`tcc_ir_ssa_opt_run()`. Goal: delete the flat duplicate, keep the SSA analog.

Next beat-HEAD fusion work (new session): see
[`plan_ssa_bitfield_fusion_next.md`](plan_ssa_bitfield_fusion_next.md) — (1) SBFX
signed extract (new opcode), (2) Case 3 bitfield-CMP cross-reload bases. Do #2 first.

Out of scope (not listed): post-RA passes (SSA gone), ARM machine fusions
(`lea_*`, `pack64*`, `gens_*`, `bfi`/`ubfx`, …), and frontend memory-init
lowering (`*_memset_to_store`, `memmove_*`, `block_copy`).

## Method (per pass, one at a time)

1. Remove the call site (leave the function body for a later cleanup sweep).
2. `make cross` + `make test-ir` — must stay 13491 passed, 0 failed.
3. Fuzz differential (`diff_olevels`, or object-diff subsumption) — no new divergence.
4. Green → keep removed. Red → revert; the delta shows what the SSA analog misses.
5. After a batch is finalized: delete dead function bodies, prototypes, unit tests.

## Tracker: flat-IR pass → SSA analog

Propagation / constants:
- [x] `copy_prop` → `ssa:cprop` / `ssa:var_forward`: **partialy**, flag still exists since more loops are guarded by it
- [ ] `const_prop` → `ssa:sccp` / `ssa:cprop`: **in progress** (extend-then-remove). Measured
  gap = force-disable `const_prop`, `compare_worktree --baseline-commit HEAD --opt o2`: **+2,609**
  (arithmetic/identity core already in `ssa:fold`; residual = bitfield cluster `gcc-execute/20040709-*`
  + cross-block propagation reach). Bricks ported into SSA so far:
  - `div_by_zero_trap`/`mod_by_zero_trap`: `ssa:fold` now rewrites const-0-divisor DIV/UDIV/IMOD/UMOD
    to `TRAP` + NOPs the block tail so branch-reachability/phi-prune/dce collapse the dead remainder
    (ssa_opt_fold.c). `ssa:fold` previously bailed (`return 0`) on div-by-zero. Gap **+2,609 → +2,470**.
  - **legacy `const_prop` now force-disabled** (`return 0` in `tcc_ir_opt_const_prop`); driving SSA to
    close the residual. Known orthogonal regression exposed: `tstdi-1` (64-bit SELECT inline-cond
    encoding bug, const_prop masked it via the pool-operand SELECT path) — fixed separately in codegen.
  - `var_imm_prop` 64-bit reach: `ssa_opt_var_imm_prop` (ssa_opt_cprop.c) was INT32-only; now also
    forwards single-def INT64 VAR consts (`ASSIGN #imm64`) to dominated uses, gated single-use
    (per-VAR use_cnt) to mirror legacy's `use_count>1 && NEEDS_POOL_LOAD` suppression — single-use
    kills the def (strict win). Width-matched (no 64↔32 truncation forwarding).
  - `var_imm_prop` / `var_const_fold` accept slot-`STORE` immediate defs (not just `ASSIGN`),
    matching legacy's `ASSIGN|STORE` def acceptance (opt_constprop.c ~2043). In `var_const_fold` the
    backward alias scan now lets a `STORE` targeting the folded VAR through to capture (it was
    aborting on all stores). Note: single-def VAR-vreg `STORE`-imm defs are rare in this frontend
    (scalars → `ASSIGN`, aggregates → DEREF/StackLoc stores), so low practical impact.
  - Both passes now block `is_volatile` VAR slots (mirrors ssa_opt_dce.c DSE guard). See
    [bugs/volatile-local-folded-to-constant.md](bugs/volatile-local-folded-to-constant.md) — the
    volatile-local-folded-to-const miscompile is broader (also flat pipeline / IR-gen), tracked there.
  - **Bitfield self-compare CMP fold (`ssa:branch` Case 3)** — the `20040709-*` cluster. `x=s;
    if (x.f != s.f) abort();` lowers `x.f`/`s.f` asymmetrically at SSA time (`UBFX(V,lo,w)` vs
    `SHL #a` completed by the CMP's own `LSR/ASR #b` barrel — ARM CMP shifts src2 only), so GVN
    can't coalesce them. `ssa_cmp_extract_desc` decodes each operand (incl. the barrel it reads
    directly, so a missing barrel just fails to match) into `(base,lsb,width,sext)`; when both match,
    the compare is reflexive → fold. Cascades: once the abort branch dies, load_cse forwards the
    other fields' `GlobalSym` reloads to the same base and they fold too. Net gap **+2,470 → ~+1,900**;
    functional tests pass.

  - **`var_imm_prop` accepts lval-STACKOFF VAR-slot reads** — resolved `gcc-execute/strlen-4`
    (+~515→largely closed). Root cause found by instrumenting `var_imm_prop_slot`: a VAR's value read
    into an arithmetic op (e.g. `V1 MUL #28`, the array-stride) is encoded as an **lval STACKOFF**
    (`tag=STACKOFF, is_lval=1, is_local=1`), not a bare VREG — and the pass's first guard rejected all
    lval/local operands, so a resolved single-def const VAR was never forwarded into its uses. Now
    accepts `STACKOFF && is_lval && !is_sym` (the slot value load) in addition to bare non-lval VREG,
    same acceptance `ssa_opt_var_const_fold` already had. Kept the INT32/INT64 width-match + single-def
    + non-addrtaken/volatile guards → whole-scalar reads only. Broad reach change (folds VAR-slot-value
    reads corpus-wide); needs full behavioral + metric verification.
- [~] `const_prop_tmp` → `ssa:const_prop_tmp` — **SSA analog landed 2026-07-10 (faithful full
  port); flat pass still runs, removal pending gap measurement.** Method: exposed the flat pass's
  ungated core `tcc_ir_opt_const_prop_tmp_core` (ir/opt.h; renamed from static `__timed`) and wired
  a thin SSA wrapper `ssa_opt_const_prop_tmp` (end of ir/opt/ssa_opt_cprop.c) that calls the core +
  `tcc_ir_ssa_opt_rebuild` on change — single source of truth, zero divergence. Runs in BOTH SSA
  drivers (ssa_opt.c main loop + regalloc.c no-promotable fallback) right after `ssa:var_imm_prop`,
  under the `ssa:const_prop_tmp` disable knob; UT stub added to ra_link_stubs.c. Safe on SSA-form
  IR: only substitutes immediates / folds-NOPs individual ops (incl. SWITCH_TABLE const-index→JUMP
  and soft-FP `__aeabi_c[df]cmp[le|eq]` both-immediate folds — two transforms SSA lacked), never
  moves a def across blocks; its branch folds only ever *remove* CFG edges, leaving `ctx->cfg`
  conservatively stale exactly like `ssa:branch` already does. `make test` green; `compare_worktree`
  shows a small improvement vs base (the SSA copy runs *after* RA-prep + SSA construction, so it
  catches a few constants the early flat pass can't yet see).
- [ ] `const_var_prop` → `ssa:var_const_fold` / `ssa:var_imm_prop` — **partial**: single-def
  imm/slot-STORE VAR consts forwarded (lval-STACKOFF slot reads accepted, 64-bit single-use); the
  multi-def block-local VAR tracking `const_prop_tmp` does is now also reachable via the shared core.
- [ ] `value_tracking` → `ssa:sccp` / `ssa:fold`
- [ ] `known_bits` → `ssa:sccp` / `ssa:fold`

Forwarding:
- [ ] `var_tmp_fwd` → `ssa:var_forward`
- [ ] `deref_fwd` → `ssa:var_forward`
- [ ] `addrof_var_fwd` → `ssa:var_forward`
- [ ] `global_sl_fwd` → `ssa:var_forward`
- [ ] `ptr_store_load_fwd` → `ssa:load_cse` / `ssa:var_forward`
- [ ] `diamond_store_fwd` → `ssa:load_cse`

CSE:
- [x] `ptr_load_cse` → `ssa:load_cse` - **removed** huge improvement
- [x] `lea_cse` → `ssa:gvn` — **removed** (net 0/416 delta; regression fixed). gvn now value-numbers
  `TCCIR_OP_LEA` of a vreg-backed stack slot (STACKOFF with `vreg < -1`, an anonymous temp local)
  via `gvn_try_lea`, dominator-scoped, converting redundant LEAs to `ASSIGN dest <- canonical` for
  cprop to forward. `vreg == -1` STACKOFFs are left to `lea_fold` (its single-use precondition would
  break under CSE), matching the legacy gate. Key gotcha: these operands are STRUCT-typed (vector),
  so `u.imm32` packs a per-reference `ctype_idx` in its low 16 bits over the real offset in
  `aux_data` — the key must use `irop_get_stack_offset()`, exactly as legacy `lea_cse_operand_equal`
  did, or no two references ever match.
- [x] `bool_cse` → `ssa:gvn` — **removed** (codegen-neutral: 0/416 delta; gvn already covers it).
- [x] `cse_param_add` → `ssa:gvn` — **removed.** regression fixed, overall it was win, we accepted wide instruction loss in favour of total delta win
- [ ] `globalsym_cse` → **not a gvn shape.** 11/416 delta. CSE of *inline SYMREF address
  materialization* (repeated `ldr rN,[pc,#off]` literal-pool loads of a global address). No
  instruction for gvn to number; needs a dedicated SSA rematerialization rule (`ssa:symaddr_cse`).
- [x] `local_alu_cse` → `ssa:gvn` — **removed** (net −106). Two gvn gaps closed to get there:
  (1) SYMREF operands were keyed by pool index (never deduplicated) — now keyed by resolved
  `(sym, addend)`; (2) VAR/lval/multi-def sources were rejected outright — now value-numbered in a
  block-local cache with the legacy kill rules (kill on source-vreg redef, on stores/addrtaken
  writes for lval sources, flush on calls/BLOCK_COPY/INLINE_ASM). Non-lval vreg-less STACKOFF
  (`Addr[StackLoc[-N]]`) additionally allowed as a stable key. Residual: 110(+4), 254(+32) — NOT
  missed CSE: disasm shows equal-or-fewer instructions (110 is one shorter) but narrow→wide Thumb
  encoding flips (14 in 254, 3 in 110) from regalloc placing values in r8–r12 where 16-bit forms
  need r0–r7. Fixing that class = regalloc low-reg preference for narrow-encodable ops (separate
  work item, not SSA) — DONE 2026-07-07: `ra:narrow_pref`, net −1324 on the corpus, 110 now −16;
  see docs/regalloc_narrow_pref.md. Two recovery attempts measured and rejected: constant-remat CSE
  (`T <-- #imm` dedup) at dominator scope net +976, block-local net +46 vs −106 baseline — shared
  constant registers cost more in pressure/hints than the saved movs.

DCE / DSE:
- [ ] `dse` → `ssa:dce`: **ongoing** legacy pass still needed post register allocator  
- [ ] `store_redundant` → `ssa:dce`
- [ ] `dead_var_store_elim` → `ssa:dce`
- [x] `redundant_init_elim` → `ssa:dce` — **removed** (net −2388). Plain removal regressed +200/11
  objects: ssa:dce only killed whole-function-dead VARs, not defs overwritten before use on every
  path. Closed generally with `dce_var_liveness` (ssa_opt_dce.c): CFG-wide backward bitset liveness
  over local VAR slots on a freshly built CFG (ctx->cfg can be stale after branch rewrites); any
  full-slot def of a non-address-taken, non-volatile VAR that is dead at its point is NOPed —
  covers overwrites on any path, not just function-entry inits. Kills are only side-effect-free
  full-width defs (STORE slot-def form included; 64-bit vars need a 64-bit def; STRUCT btype and
  side-effect-op dests count as uses, i.e. conservative). Bails on IJUMP/setjmp/asm/static-chain/
  builtin-apply. Knob: `TCC_DISABLE_PASS=ssa:dce:var_live`. Residual 5 larger (+48 total,
  pr91190 +24) is regalloc register-choice drift, not missed DSE. Fuzz verification deferred.

Forwarding (drivers): `sl_forward`, `entry_store_prop`/`esp_cleanup` — the memory-group store→load
forwarding drivers. Same `ssa:var_forward` / `ssa:load_cse` target as the scalar forwarders above;
none removed. High value (largest remaining cluster) but load-bearing — do after the propagation
cluster is deletable so the flat pipeline isn't relied on to feed them.

Extend-first landed elsewhere (SSA analog live, flat duplicate still runs):
- [~] `string_calls` → `ssa:const_string_fold` (source/opt/ssa/const_string_fold.c) — strlen/strcmp/
  memcmp/strcpy/strspn/strcspn/strchr/strrchr/strstr/strpbrk/index/rindex + clrsb const folds landed.
- [~] `stack_bool` → `ssa:or_bool_diamond` / `ssa:stack_bool_diamond` — landed.
- (new, not a flat-pass replacement) `ssa:global_addr_hoist` — parks a hot global address in a
  callee-saved reg across calls; runs post-phi-resolution in regalloc.c. No `globalsym_cse` analog.

Specialized folds — still flat, **never inventoried against SSA** (candidate analog / gap unknown;
measure per-pass with `TCC_DISABLE_PASS=<flat> compare_worktree` before porting):
- [ ] `add_reassoc` → candidate `ssa:reassoc` (already exists — likely closest to deletable; verify)
- [ ] `neg_chain_cse` → candidate `ssa:gvn`
- [ ] `cmp_expr_fold`, `cmp_offset_fold`, `cmp_field_fuse` → candidate `ssa:branch` / `ssa:cmp_eq_prop`
- [ ] `self_arith`, `self_copy_elim`, `single_val_tmp` → candidate `ssa:fold` / `ssa:cprop`
- [ ] `setif_fuse`, `setif_or_taut` → candidate `ssa:branch`
- [ ] `switch_collapse`, `stack_nonnull` → candidate `ssa:branch` / `ssa:sccp` (const-index switch
  resolution is now also in `ssa:const_prop_tmp`)
- [ ] `bf_insert_extract` → candidate `ssa:narrow` (UBFX fusion already there — see plan_ssa_bitfield_fusion_next)
- [ ] `float_branch`, `float_narrow` → candidate `ssa:branch` (soft-FP cmp) / `ssa:narrow`
- [ ] `vrp` → no SSA range-propagation pass exists; likely a genuine new pass
- [ ] `const_agg_fold` → aggregate-RMW slot forwarding across calls; no SSA analog (complex; may stay)
- [ ] `return_reuse`, `redundant_assign` (late-cleanup copy) → candidate `ssa:dce` / backend

Out of scope — stay flat by design (do NOT port):
- ARM machine fusions (`fusion_mla`, `deref_indexed`, `disp_fusion`, `chain_fold`, `pair_reorder`,
  `bool_simplify`) — pattern-match physical/TEMP shapes, not an SSA concern.
- VLA / alloca lowering (`dead_vla_struct`, `zero_vla`, `alloca_load_fwd`, `dead_alloca_vreg`) and
  frontend memory-init lowering (`*_memset_to_store`, `memmove_*`, `block_copy`).
- All **post-RA** passes (SSA is gone by then): `dse` post-RA cleanup, post-RA `jump_threading` /
  `orphan_cmp`, branch-size opt, etc.

Note: analog names are candidates, not proven subsumption — step 2/3 decides.
`dce` itself is retained (used as a cleanup cascade by many sites, incl. post-RA).
