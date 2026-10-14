# A packed `_Complex float` member is read with `LDRD` and faults

**Status:** open · **Severity:** correctness (HardFault, every level) · **Found:** 2026-10-14 (probing the `lea_fold` complex-width fix, branch `fix5/lea-atomic`) · **Reported by:** claude-opus-5-5

## Summary

A `_Complex float` member of a `__attribute__((packed))` struct sits at an
unaligned offset, but passing it by value loads both halves with one `LDRD`
(`ldrd r0, r1, [r4]` with `r4 = p + 1`). `LDRD` needs word alignment on
ARMv8-M, so the program takes a UsageFault (CFSR `0x01000000`, UNALIGNED)
escalated to a HardFault. A packed `long long` or `double` member is read with
two `LDR`s, so the underalignment check exists for 64-bit scalars but not for
the complex pair. gcc reads the halves with `ldr.w r2, [r3, #1]` /
`ldr.w r1, [r3, #5]`.

## Reproducer

`docs/bugs/repro/packed-complex-member-ldrd-unaligned.c`

| | gcc | tcc `-O0`/`-O1`/`-O2`/`-Os` |
|---|---|---|
| output | `55 55` | HardFault in `viapk` |

The store side (`p->z = v`) uses `strd r1, r2, [sp]` on the frame, which is
aligned; the store through the packed pointer has not been checked.

## Root cause

Not pinned down. The by-value argument load of a complex operand goes through
the 64-bit pair paths in `source/backend/arch/arm/thumb/arm-thumb-call.c`
(`try_ldrd_pair` / `tcc_gen_machine_try_ldrd_base`) or `arm-thumb-mem.c`;
the packed `long long` path consults `align4` / `underalign_hint`
(`IROP_AUX_UNDERALIGN`), so either the complex operand never receives the
underalign mark from the frontend (`vstack.c` sets it for packed members), or
the complex branch skips the check. `arm-thumb-mem.c:105` explicitly excludes
`is_complex` from its own LDRD path, which suggests the complex load is
handled elsewhere.

## Regression lock

None.

## Likely fix

Carry `IROP_AUX_UNDERALIGN` onto packed complex members and make every LDRD/STRD
pair path used for a complex operand check `align4 && !underalign_hint`, as
the 64-bit scalar paths do.
