# Plan: merge switch case bodies that differ only in their own case constant

Status: landed as 0512fba8 (2026-09-27).

Corrections found while implementing:

- The `SWITCH_TABLE` operand is `sel - min_val`, not the selector. Look through the
  `idx <-- X SUB #min_val` right before the dispatch and use X.
- `alive_share` can give the selector's register to another value inside a body where
  the selector is dead. A define scan of the selector vreg is not enough: scan every
  operand of the kept copy for a register or slot overlapping the selector's post-RA
  location, and require the selector in a callee-saved register (r4-r11) or a slot.
Owner: whoever picks up `source/ir/cross_jump.c` after e8501cf9.

## Problem

`tcc_ir_region_merge` (post-RA, `cross_jump.c`, landed e8501cf9) folds switch
case bodies that are byte-identical. In Zig-rendered C the dominant near-miss is

```c
switch (x.tag) {
  case 4: { ... r.tag = 4; ... }   /* body identical to case 5 except the 4 */
  case 5: { ... r.tag = 5; ... }
}
```

The constant written in case k is k itself, i.e. the switch selector. The
matcher stops at that instruction (debug reason 2, `T <-- #4 [LOAD]` vs
`T <-- #5 [LOAD]`), so 79 bodies of `Value_interpret__anon_196470__49504` and
91 of `InternPool_PackedCallingConvention_pack__30991` stay separate. clang gets
this through SimplifyCFG sink-with-phi followed by `ForwardSwitchConditionToPHI`;
its merged body stores the tag from the selector register.

Dry run (patch `.cache/zigmem/build/sw/rm_dry.patch`, `TCC_RM_DRY=1`, tally line
`RMDRY <fn> pairs= insns=`) on zig.c: 14 functions hit, ~30 KB of 5.24 MB:

| function | est. removed | of |
|---|---|---|
| Value_interpret__anon_196470 | 16.6 KB | 24.8 KB |
| InternPool_PackedCallingConvention_unpack | ~7 KB | 7.6 KB |
| InternPool_PackedCallingConvention_pack | 7.3 KB | 7.6 KB |
| 11 others | < 300 B each | |

The dry run leaves every other function byte-identical to the baseline (one
function, `zig_system_glibcVerFromRPath`, is 8 B larger because a virtually
retired body blocked a later exact merge; the real transform does not have
that problem since the retired body is really gone).

## Design

All work is inside `rm_match` / `tcc_ir_region_merge`. Nothing changes for
pairs that match exactly.

### 1. Per-entry case value (setup, once per function)

In the counting loop that already fills `st.trefs` from each `SWITCH_TABLE`,
also fill `int64_t caseval[n]`:

- `RM_NOCASE` initially;
- `tab->min_val + e` for `tab->targets[e]` when the slot is still `RM_NOCASE`,
  `RM_MULTI` if a second entry or the default lands on the same index;
- after the loop, `RM_MULTI` for any index with `st.jrefs[i] != 0` (a plain
  jump also reaches it) or that the previous non-NOP instruction can fall into
  (accept only `JUMP`, `RETURNVALUE`, `RETURNVOID`, `SWITCH_TABLE` before it).

Also remember, per index, which `SWITCH_TABLE` instruction dispatches there
(`selsw[i]`), so the selector operand can be copied from it later.

Only `SWITCH_TABLE` dispatch is covered. A compare chain (`CMP sel,#k; JUMPIF`)
also implies `sel == k` on the taken edge, but the target of a `JUMPIF` has
`jrefs`, and nothing in zig.c measured needs it. Leave it out of the first cut.

### 2. Match rule (in `rm_match`)

When `rm_insn_same(qa, qb)` fails, try `rm_insn_same_case(st, a, b, qa, qb)`:

- same op, `rm_op_ok`, not a jump, not a call/param op;
- exactly one operand slot differs, both operands `IROP_TAG_IMM32`, not
  struct-typed, every other operand flag equal (`btype`, `is_unsigned`, `aux`,
  … the same list `cj_operand_same` checks);
- `imm_a == caseval[a]` and `imm_b == caseval[b]`, comparing both as int32 and
  as uint32 against the int64 case value;
- the side tables `rm_insn_same` compares (barrel shift, shift64 dead half,
  zero_half64, bfi params) equal.

Record the hit: `st->param_hit = 1`, and push `(ia, slot)` onto a small list
`st->param_at[]` (several instructions in one body may carry the constant, for
instance a narrow store and a later reload; allow up to, say, 8 per region).

The selector must be 32 bits or narrower. Reject when the `SWITCH_TABLE`
source operand is `IROP_BTYPE_I64` (a u64 selector cannot be copied into an
IMM32 slot).

### 3. Selector availability at the use

Before accepting a parameterised match, check that the selector still holds
the case value at each recorded instruction in **both** copies. Two shapes
occur:

- **vreg selector** (`PackedCallingConvention_pack`: `T13807 <-- T4 UBFX #256`,
  held in r4). tcc emits the dispatch after all bodies, so the selector's live
  interval spans every body in linear order and no body redefines it. Check it
  anyway: no instruction in `[entry, use]` of either copy defines that vreg
  (`cj_defines`), and `tcc_ir_get_live_interval(ir, sel)` covers the use index
  with a single location (register or spill slot). If the RA split the interval
  and the location at the use differs from the location at the dispatch, skip.
- **VAR selector** (`Value_interpret`: `t26` is a local loaded with `ldrb` at
  the dispatch). Value at the body entry equals the value at the dispatch,
  because the dispatch jumps straight to the entry. Check that no `STORE` /
  `STORE_INDEXED` / `ASSIGN` / `BLOCK_COPY` / memcpy-style call in
  `[entry, use)` of either copy has that VAR as destination, and that the VAR
  is not address-taken in the function (any `LEA`/`&V` of it anywhere → skip;
  a pointer could alias it).

If any check fails, treat the pair as a non-match (return 0), exactly as today.

### 4. Rewrite (in the driver, before deleting copy B)

For each recorded `(ia, slot)` in copy A: replace the IMM32 operand with a
verbatim copy of the `SWITCH_TABLE`'s source operand (`tcc_ir_op_get_src1` of
`selsw[a]`). The op stays what it was:

- `T <-- #k [LOAD]` becomes `T <-- Tsel [LOAD]` (a register move; the same
  form the RA already emits for register-passed PARAMs, `regalloc.c` ~1274) or
  `T <-- Vsel [LOAD]` (a slot load with the dispatch's own width).
- `V <-- #k [STORE]` becomes `V <-- Tsel [STORE]` / `V <-- Vsel [STORE]`.
  If the backend rejects a VAR as a STORE source post-RA, skip such pairs in
  step 2 (only accept `LOAD` dests and register-source stores); both measured
  functions carry the constant through a `LOAD` first.

Then delete copy B and retarget its entry exactly as the exact-match path does.
Later candidates C against A compare `Tsel` (A) with `#k_c` (C): extend
`rm_insn_same_case` to accept "A's operand is already the selector operand
and C's constant equals `caseval[c]`" so the third and following bodies fold
into the same copy.

Update `st.occ` for the selector's vreg key when a new use is added, since the
one-to-one value pairing counts occurrences.

### 5. Knobs and debug

- `TCC_DISABLE_PASS=region_merge:case_const` disables only the new rule.
- Keep the `TCC_RM_DBG=<func>` reporter from `rm_debug.patch` (prints
  `RM a= b= k= why= at=`), adding `why=14` for "constant differs but is not the
  case value" so future near-misses are visible.

## Implementation steps

1. Land the dry-run scaffolding as real code: `caseval[]`, `selsw[]`,
   `rm_no_fallthrough`, `rm_operand_same_but_imm`, `rm_insn_same_case`
   (from `rm_dry.patch`, minus the tally).
2. Add the availability checks of §3 (vreg define scan, VAR store scan,
   address-taken scan).
3. Add the rewrite of §4 and the "already the selector" comparison.
4. Unit test in `tests/unit/arm/armv8m/` (model: `test_opt_switch_to_data.c`,
   builder `ir_build.h`): hand-build a `SWITCH_TABLE` with three targets whose
   bodies are `T <-- #k; V <-- T [STORE]; JUMP end`. Assert: return value 2
   (two bodies removed), A's first instruction now reads the selector operand,
   B's and C's entries are single `JUMP`s to A. Negative tests: a body whose
   constant is `k+1` (no merge), an entry also reached by a plain `JUMP` (no
   merge), a VAR selector stored inside the body (no merge), a u64 selector.
5. `tests/ir_tests/` program: a 6-case switch returning `(struct){.tag = k,
   .payload = f(k)}` through a pointer, plus a default; prints the tags and
   `PASS`. Same program compiled with the knob off must print the same.
6. Validation, in this order: `make test` IR suite (14,465 expected; 6 known
   YAFF-v4 reds), torture (11,228/0), `check_zig` on the Linux build
   (`.cache/zigmem/agg/check_zig_z0.sh`; compare on-vs-off C output, md5s of
   different renders do not match), then the QEMU device suite via
   `./scripts/run_qemu_smoke.sh --no-build tcc_suite_test.py` after the rootfs
   rebuild (the native tcc is compiled by the cross, so a codegen bug here
   shows up as a self-host miscompile; see
   `docs/selfhost_miscompile_debugging.md`).
7. Measure on zig.c with `.cache/zigmem/build/sw` recipe: expect
   `Value_interpret__anon_196470` ≈ 8 KB, both `PackedCallingConvention_*`
   ≈ 1 KB, object −30 KB. `pack` should end up below clang -Oz (3.6 KB),
   which does not merge these 91 bodies.

## Risks

- **Selector clobbered inside the body.** Covered by the define/store scans;
  the dry run did not need them only because no rewrite happened. Do not skip
  them.
- **Aliased VAR selector.** Zig renders the selector as a local loaded from an
  error-union temp; if its address escapes, a call in the body could change it.
  The address-taken scan is the guard.
- **Width.** The dispatch compares the full register value (`CMP sel,#max`
  then table index), so in case k the 32-bit selector value is exactly k. The
  constant may be typed narrower (u8 tag); copying the selector operand keeps
  the dispatch's width, and the consuming instruction (narrow store, or a
  32-bit op) sees the same bits it saw with the immediate. A *signed* narrow
  selector with negative case values is handled by comparing the immediate both
  sign- and zero-extended; when in doubt reject `is_unsigned` mismatches.
- **Occurrence counts.** `rm_match` requires each paired value to have all its
  occurrences inside the two regions; adding a use of the selector breaks that
  for the selector itself. Exclude the selector vreg from that check (it is
  live across the whole function by construction).

## What this does not do

It is a ~30 KB, three-function win. The remaining repeated text in zig.c
(1.09 MB within functions, 1.95 MB counting across functions, by 8-instruction
windows) is inlined callee bodies with private frame slots, block copies,
no-op same-slot copies and cross-function idioms; see the analysis recorded in
the agent memory note `tcc-zigc-size-vs-clang-oz-2026-09-27` (section
"repeated text decomposition").
