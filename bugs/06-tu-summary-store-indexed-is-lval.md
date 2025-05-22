# 06 — `collect_tu_func_summary` missed STORE_INDEXED / STORE_POSTINC writes when `is_lval` was cleared

**Status:** FIXED in this branch ([ir/opt.c:822-844](../ir/opt.c#L822-L844))
**Severity:** Medium — silently prevented end-of-TU dead-static-store elimination.

## Symptom

`tcc_ir_collect_tu_func_summary` recorded a write to a static global only
when the STORE dest carried both `is_sym=1` and `is_lval=1`:

```c
if (q->op == TCCIR_OP_STORE || q->op == TCCIR_OP_STORE_INDEXED ||
    q->op == TCCIR_OP_STORE_POSTINC) {
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  if (dest.is_sym && dest.is_lval) { ... }   // <-- too strict
}
```

But `disp_fusion` may have cleared `is_lval` on the *base* operand of
`STORE_INDEXED` / `STORE_POSTINC` (see comment in [ir/opt_fusion.c:1925-1928](../ir/opt_fusion.c#L1925-L1928): *"disp_fusion clears
is_lval on STORE_INDEXED's base, so the is_lval test alone would
mis-classify it as a redef"*). Result: writes to a static global through
an indexed/postinc form were silently dropped from the summary.

## Repro

`tests/gcctestsuite/.../gcc.c-torture/compile/pr25483.c` at `-O2`.
`decode_init` writes `mdct_win[j] = (int)(d * 3)` inside a loop. After
fusion, the IR contains:

```
0019: GlobalSym(1182) <-- R0(T6) STORE_INDEXED R6(T7)
```

The summary collector saw `dest.is_lval=0` (cleared by `disp_fusion`) and
skipped the entry, so `mdct_win` never appeared in `static_writes`.
Without that record, [[08]]'s `tcc_ir_tu_analyze_dead_statics` could not
mark `mdct_win` as `tu_no_readers` and `decode_init` was never
re-optimized.

## Fix

Relax the `is_lval` check specifically for the indexed/postinc forms —
their dest *is* the memory write target regardless of the flag:

```c
int dest_is_write_target =
    dest.is_sym &&
    (dest.is_lval || q->op == TCCIR_OP_STORE_INDEXED ||
     q->op == TCCIR_OP_STORE_POSTINC);
```

## Related

- [[07]] — the same is_lval over-restriction affected `dead_static_store_elim` itself.
- [[08]] — the late_reopt mechanism that this summary feeds.
