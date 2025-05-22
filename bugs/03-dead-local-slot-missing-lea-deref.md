# 03 — `dead_local_slot_elim` ignores STOREs via LEA temp deref

**Status:** FIXED in this branch via new pass [ir/opt_dead_lea_store.c](../ir/opt_dead_lea_store.c)
**Severity:** Medium — leaves dead bitfield writes after upstream chains collapse.

## Symptom

`dead_local_slot_elim` ([ir/opt_memory.c:4406-4441](../ir/opt_memory.c#L4406-L4441))
only NOPs STOREs whose `dest` operand is a **direct** `StackLoc[X]` form:

```c
if (q->op != TCCIR_OP_STORE) continue;
IROperand dest = tcc_ir_op_get_dest(ir, q);
if (irop_get_tag(dest) != IROP_TAG_STACKOFF) continue;
if (!dest.is_local || irop_get_vreg(dest) != -1) continue;
```

It silently skips the equally common temp-deref form:

```
T0 <-- Addr[StackLoc[-4]]
T0***DEREF*** <-- T2 [STORE]
```

The `live[]` collection at [ir/opt_memory.c:4273-4342](../ir/opt_memory.c#L4273-L4342) has the same
asymmetry — temp-deref reads aren't registered either, so even the
elimination logic that *does* fire is working from an incomplete picture
of which slots are live.

## Repro

bitfld-1 after the [[02]] workaround folds all the bitfield extractors —
the IR collapses to just the two bitfield-insert STOREs:

```
0007: R0(T3)***DEREF*** <-- R2(T5) [STORE]   ; never read again
0008: RETURNVALUE #0
```

`dead_local_slot_elim` walks past those STOREs (dest tag != STACKOFF),
the stack frame stays, the bitfield computation stays. Final size:
15 instructions vs GCC's 2.

## Fix

New pass [ir/opt_dead_lea_store.c](../ir/opt_dead_lea_store.c):

1. Identify single-def TEMPs whose RHS is `Addr[StackLoc[Y]]`
   (single-def required so the slot mapping is stable; lval dests are
   skipped from the def count — that's the gotcha from [[05]]).
2. Resolve both STORE dests and lval-source reads through that map,
   so the temp-deref form participates in liveness.
3. Eliminate a STORE whose byte range is never read by a later instruction.

Conservative bails: any IJUMP / SETJMP / INLINE_ASM / VLA in the function,
any non-mem* CALL, any escape of the address to a VAR/PARAM or untracked
TEMP, any mem* `PARAM1` (the source side) with unknown size or unknown
source. The existing `dead_local_slot_elim` does similar tameness work
for the direct-stack-ref form — extending its 1500-line implementation
to also recognize the temp-deref shape was deemed higher risk than a
narrower companion pass.

## Why both passes?

The two forms cover different upstream sources:

- Direct `STORE StackLoc[X]` form arises after `sl_forward` canonicalizes
  a `LEA + STORE T_DEREF` pair — `dead_local_slot_elim` handles these.
- Temp-deref `STORE T0_DEREF` form survives when `sl_forward` doesn't
  canonicalize (the LEA temp is reused, has multi-use shape, etc.).
  The new pass handles these.

A future refactor could unify both into one pass with a slot-resolver
helper, but the current split keeps each pass small and obviously sound.

## Related

- [[02]] — without `known_bits` the downstream reads of the slot don't go away, so this pass would correctly leave the STOREs alive.
- [[05]] — gotcha that bit the first attempt at this pass.
