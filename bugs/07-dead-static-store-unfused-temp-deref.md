# 07 — `dead_static_store_elim` missed the pre-fusion `T = ADD(SYMREF, …); *T = v` form

**Status:** FIXED in this branch ([ir/opt_memory.c:5336-5440](../ir/opt_memory.c#L5336-L5440))
**Severity:** Medium — pass was effectively a no-op for static-array writes.

## Symptom

`dead_static_store_elim` looked for the *post-fusion* shape only:

```c
IROperand dest = tcc_ir_op_get_dest(ir, q);
if (!dest.is_sym || !dest.is_lval) continue;
```

i.e. it required the STORE dest itself to be a `SYMREF` operand. But
during the IR optimization pipeline, the canonical form of a static-array
write is still:

```
T_addr = ADD(SYMREF, scaled_index)     ; or LEA / ASSIGN of SYMREF
*T_addr = value                        ; STORE through TEMP, dest=lval TEMP
```

The fusion from "TEMP-DEREF STORE" to "STORE_INDEXED with SYMREF base"
runs during machine_op / codegen translation, **after** the late_cleanup
pass group has already run. So in practice, the pass never matched a
real-world write to a file-scope static array — it was only fixing
direct `static_int = 0` style scalar writes.

## Repro

`tests/gcctestsuite/.../gcc.c-torture/compile/pr25483.c` at `-O2`:

```c
static int mdct_win[8];
int decode_init(double d) {
  int j;
  for (j = 4; j; j--) { d *= 0.5; mdct_win[j] = (d * 3); }
}
```

IR in the late_cleanup phase (pre-codegen):

```
0011: T3 <-- V0 SHL #2
0012: T4 <-- GlobalSym(1182) ADD T3        ; T4 = &mdct_win[j]
0018: T4***DEREF*** <-- T6 [STORE]          ; *T4 = (int)(d*3)
```

`dest=T4` is a TEMP, not a SYMREF, so the pass skipped the STORE even
though `mdct_win` was correctly marked `tu_no_readers`.

## Fix

Add an indirect-resolution helper that, when dest is a single-def lval
TEMP, traces back to the TEMP's defining `ADD`/`LEA`/`ASSIGN` and pulls
the SYMREF from `src1`:

```c
static Sym *dss_resolve_store_dest_sym(TCCIRState *ir, IRQuadCompact *q,
                                       int store_idx) {
  IROperand dest = tcc_ir_op_get_dest(ir, q);
  if (dest.is_sym) { ... handle direct form ... }
  if (q->op != TCCIR_OP_STORE || !dest.is_lval) return NULL;
  /* TEMP-DEREF: trace back to single-def ADD/LEA/ASSIGN of SYMREF */
  ...
}
```

Constraints kept tight to stay sound: single-def TEMP only, no other
defs anywhere in the function, src1 must be a non-lval SYMREF.

## Why it matters (cascade)

NOPing the STORE alone is small; the win is what DCE drops afterward.
For pr25483, NOPing the STORE_INDEXED to `mdct_win` lets DCE remove the
chain feeding it:

- `T6 = CALL __aeabi_d2iz(T5)` — pure aeabi call, result now dead
- `T5 = CALL __aeabi_dmul(d, 3.0)` — pure aeabi call, result now dead
- `T3 = SHL V0, 2` and `T4 = ADD(mdct_win, T3)` — address dead

Final result: 30 instructions → 16 instructions for `decode_init`.

## Related

- [[06]] — companion is_lval over-restriction in the summary collector.
- [[08]] — without late_reopt firing at all, this pass wouldn't run on pr25483 regardless.
