# Hard-float: a struct argument's LDM overwrites a double argument staged in r0:r1

**Status:** open · **Severity:** correctness (miscompile, `-mfloat-abi=hard`, every level) · **Found:** 2026-10-14 (adversarial tests for `fix5/knr`; base `f6d53136` too) · **Reported by:** claude-opus-5-5

## Summary

With `-mfloat-abi=hard`, a call that passes a struct by value in core
registers and a `double` that is itself the result of an earlier call
passes the struct's words as the double:

```c
int ks(struct S s, int c, double f);           /* s in r0-r2, c in r3, f in d0 */
... ks(s, arg(-1), dbl(1.5)) ...
```

```
bl   dbl
vmov r0, r1, d0          @ the double is staged in r0:r1
add  r2, sp, #4
ldmia r2, {r0, r1, r2}   @ the struct overwrites r0-r2
mov  r3, r4
vmov d0, r0, r1          @ d0 = s.a, s.b
bl   ks
```

`-O0`, `-O1`, `-O2` and `-Os` all emit this sequence. A `double` that is a
parameter of the caller (`ks(s, 1, d)`) is staged in callee-saved r4:r5 and
is passed correctly. The soft-float ABI is not affected: the double then goes
in core registers or on the stack, not through a staging pair.

## Reproducer

`docs/bugs/repro/hardfp-struct-arg-clobbers-staged-double-arg.c`, built with
`TCC_FLOAT_ABI=hard python run.py -c ... --cflags=-O1`:

| | gcc | tcc, every level |
|---|---|---|
| output | `8` | `5` |

## Root cause

Not yet traced. The call-argument marshaller in
`backend/arch/arm/thumb/arm-thumb-call.c` (struct-by-value word loads, the
LDM path around `th_ldm(base_addr_reg, regset, ...)`) writes r0-r2 without
checking that a pending VFP argument's source sits in those registers. The
double result was put in r0:r1 when it was moved off d0 so that the next
call could not clobber it, and the parallel move into the argument registers
does not treat that pair as a source.

## Regression lock

None.

## Likely fix

Treat the core-register source of a pending VFP argument as a live source
in the argument parallel move. Move it into its VFP register before loading
the struct words, or stage it in a register outside r0-r3.
