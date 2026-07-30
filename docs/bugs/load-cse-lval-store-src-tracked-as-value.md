# load_cse: pointer-deref store source tracked as the stored value

**Status:** open · **Severity:** correctness (miscompile) · **Found:** 2026-07-17
(unit tests for `source/opt/ssa/memory/load_cse.c`)

## Summary

In `ssa_opt_load_cse`'s stack store-load forwarding, a `STORE` whose source is
a pointer dereference (the fused memory-to-memory copy form, e.g.
`StackLoc[0] <- *p` or `*q <- *p`) records the source **pointer** `p` — not
the pointee `*p` — as the slot's stored value. A later `LOAD` of that slot is
then rewritten to `ASSIGN p`, yielding the address where the loaded value was
expected.

## Root cause

Two `sstore_track_vr` call sites in `gload_process_block`
([source/opt/ssa/memory/load_cse.c](../../source/opt/ssa/memory/load_cse.c))
take `svr = irop_get_vreg(src)` and track it whenever it is a TEMP, without
excluding `src.is_lval`:

- Direct stack-slot tracker — load_cse.c:1031-1032
  (`dest.tag == IROP_TAG_STACKOFF` branch):
  ```c
  if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP)
    sstore_track_vr(st, irop_get_stack_offset(dest), store_btype, svr, dest_base);
  ```
- TEMP-indir tracker (`*T <- val` resolving to `LEA(StackLoc[N])`) —
  load_cse.c:1076-1077:
  ```c
  if (svr >= 0 && TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP)
    sstore_track_vr(st, eff_off, store_btype, svr, store_base);
  ```

All three sibling value trackers in the same function reject lval sources:

- vslot tracker, load_cse.c:302 (`src.tag == IROP_TAG_VREG && !src.is_lval`)
- tvstore tracker, load_cse.c:1104 (`src.tag == IROP_TAG_VREG && !src.is_lval`)
- gstore tracker, load_cse.c:1181 (`sval.tag == IROP_TAG_VREG && !sval.is_lval`)

The two `sstore_track_vr` sites are the odd ones out.

## How the shape arises

The forward-into-store-source block at load_cse.c:975-1008 exists precisely
for `STORE StackLoc <- <lval src>` (STACKOFF or VREG source — the comment
cites ptr fuzz seed 7226), so the fused mem-copy shape reaches this code in
real compiles. When the lval-VREG source resolves to a tracked slot holding an
immediate, the source is rewritten first and all is well; when it does **not**
resolve (arbitrary pointer) or the tracked entry holds a TEMP, the raw
`is_lval` VREG source falls through to the trackers above, which misrecord
the pointer as the value.

## Observable symptom

```c
/* IR shape (fused mem-to-mem copy, p not LEA-resolvable): */
t1 = t0 + #0;             /* some unresolvable pointer */
StackLoc[0] <- *t1;       /* STORE lval(stackoff 0), lval(t1) */
t2 = LOAD(StackLoc[0]);
```

The pass rewrites the load to `t2 = ASSIGN t1` — the pointer, not `*t1`.
The same happens via the temp-indir site for
`t1 = &StackLoc[0]; *t1 = *t0; t2 = LOAD(StackLoc[0])` → `t2 = ASSIGN t0`.

## Regression lock

`tests/unit/arm/armv8m/test_ssa_opt_load_cse.c`:

- `test_stack_store_lval_vreg_src_tracked_bug` pins the load_cse.c:1031 site
- `test_temp_indir_store_lval_src_tracked_bug` pins the load_cse.c:1076 site

Both assert the CURRENT (buggy) result; flip their assertions to "no forward,
load preserved" once fixed. Not yet fixed.

## Likely fix

Add the sibling guard to both sites:

```c
if (svr >= 0 && src.tag == IROP_TAG_VREG && !src.is_lval &&
    TCCIR_DECODE_VREG_TYPE(svr) == TCCIR_VREG_TYPE_TEMP)
```

so an lval source falls into the existing `else` branch
(`sstore_invalidate_overlap` + `sstore_remove_offset` for the direct site,
`sstore_remove_offset` for the temp-indir site), which is the conservative,
sound outcome. Note the forward-into-src block at :975-1008 already handles
the cases where forwarding is actually valid, so no optimization is lost.
