# 02 — `SHL N → SHR M` peephole only handles `N == M`

**Status:** WORKED AROUND via [ir/opt_knownbits.c](../ir/opt_knownbits.c)
**Severity:** Medium — large class of missed folds on bitfield reads.

## Symptom

The peephole at [ir/opt_constprop.c:1436-1475](../ir/opt_constprop.c#L1436-L1475) handles only the
byte-/half-cast pattern `SHL #N → SHR #N → AND #mask`:

```c
if (shl_amt != shr_amt || shl_amt <= 0 || shl_amt >= 32)
  continue;
```

The bitfield-extract idiom uses **unequal** amounts:

- 7-bit unsigned bitfield at bit position 7: `SHL #18 → SHR #25`
- 7-bit signed bitfield at bit position 0: `SHL #25 → SAR #25`

These never collapse. They also can't be folded by `const_prop_tmp` alone
because the source value usually isn't fully constant — only specific bit
ranges are (from a preceding `(x AND mask) OR const` insert).

## Repro

bitfld-1's chain after the insert sequence:

```
T5 = (...) OR #115           ; bits 0..6 = 115 (= -13 in 7b sign)
T9  = T5 SHL #18
T10 = T9 SHR #25             ; expect: bits 7..13 of T5 = 61
T14 = T5 SHL #25
T15 = T14 SAR #25            ; expect: bits 0..6 sign-ext = -13
```

`const_prop` can fold neither chain. The whole abort-test ladder stays alive.

## Workaround

Added [ir/opt_knownbits.c](../ir/opt_knownbits.c) — a known-bits lattice (per-temp
and per-stack-slot `known_zero`/`known_one` masks). It propagates through
`AND`/`OR`/`XOR`/`SHL`/`SHR`/`SAR` and rewrites the op to `ASSIGN imm`
when all 32 bits become known. This covers the bitfield extract because
the relevant bits of `T5` are forced known by the preceding inserts even
though `T5`'s full value is not.

## A simpler, narrower alternative

For the unequal-shift peephole alone, generalize the existing fold:
when `shl_amt <= shr_amt`, replace with `(x >> (M - N)) & ((1 << (32 - M)) - 1)`
(`SHR` + `AND`). This won't help when the source value is partially known
but not constant — the cascade still needs known-bits — so the workaround
went the more general route.

## Related

- [[01]] — even when known_bits folds the SHL/SHR chain to a constant, the downstream IMOD needs the IMOD fold to also fire.
- [[04]] — and the resulting dead `abort()` call needs the pipeline to iterate so `sl_forward` can forward the stack store to subsequent reads.
