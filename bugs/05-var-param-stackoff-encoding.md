# 05 — VAR/PARAM operands carry `tag=STACKOFF` for their spill slot

**Status:** DOCUMENTED (footgun, not a bug per se)
**Severity:** Low for existing code; High for new pass authors.

## What surprised me

When a VAR or PARAM is referenced via its potential stack-spill encoding,
the operand has:

- `tag == IROP_TAG_STACKOFF`
- `is_local == 1`
- `is_lval == 1`
- `vreg_type != 0` (the originating VAR/PARAM index)
- `u.imm32` = the spill-slot offset (which may collide with offsets of
  real, distinct stack allocations)

This is **indistinguishable** from a real direct stack reference like
`StackLoc[-4]` (which has `vreg_type == 0`) on every field *except*
`vreg_type`.

A new pass that filters operands with:

```c
if (op.tag == IROP_TAG_STACKOFF && op.is_local && op.is_lval) { /* stack ref */ }
```

will silently treat a VAR's spill encoding as if it were a real slot.
If the pass also tracks per-stack-slot state (e.g. known-bits) and a
real STORE happens to write the *same offset*, it will load that state
when the VAR is read — and miscompile.

## How it bit me

`opt_knownbits.c`'s first cut treated `tag=STACKOFF, is_lval, is_local`
as a direct stack read. On
`tests/.../gcc.c-torture/execute/20040313-1.c`, a `V0` variable holding
`d = 0` was encoded as `StackLoc[-4100], vreg_type=VAR, pos=0`. The
array `t[1025]` happened to start at the same offset `-4100`, with
`t[0] = 1024` stored to it shortly before `d`'s read. The pass loaded
the `t[0]` known-bits value (1024) as if it were `d`'s value, computed
`d << 2 = 4096`, and folded that into a downstream address — turning
`t[d=0]` into `t[1024]`. Tests that depended on `d == 0` corrupted at
runtime.

## Suggested check for new passes

When treating a `STACKOFF` operand as a real stack slot reference:

```c
if (op.tag == IROP_TAG_STACKOFF && op.is_local && op.is_lval &&
    op.vreg_type == 0)   /* MUST: no vreg attached */
{
  /* genuine direct StackLoc[X] ref */
}
```

`vreg_type == 0` (no vreg) is the only encoding for a true direct stack
reference. Anything else is a vreg-backed pseudoreg whose offset field
is metadata about *where it would spill*, not where the program reads
from.

## Where this would help

A short comment in [tccir_operand.h](../tccir_operand.h) at the IROperand definition
documenting this case would have saved hours. The existing
`dead_local_slot_elim` already gets it right (it filters
`irop_get_vreg(op) != -1`), but the convention isn't called out
anywhere I could find.

## Related

- [[03]] — the same encoding gotcha affects the new dead-LEA-store pass; it uses the same `vreg_type == 0` guard.
