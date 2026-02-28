# Bug: `long double` parameter `+=` produces wrong result

## Test case
```
tests/gcctestsuite/gcc-testsuite/gcc/testsuite/gcc.c-torture/execute/20030914-1.c
```

## Symptom
`pc += pb.val[i]` has no effect when `pc` is a `long double` **parameter** — result stays at 10000.0 instead of accumulating to 10136.0.

## Original error (may have been fixed separately)
```
tcc_ir_vreg_live_interval: invalid vreg: -2
```
This no longer reproduces on current code. The remaining issue is pure runtime correctness.

## Reproduction
```bash
cd tests/ir_tests
python run.py -c ../gcctestsuite/gcc-testsuite/gcc/testsuite/gcc.c-torture/execute/20030914-1.c --cflags="-O1"
# Exit code: 1  (abort called because f() returns 10000.0 instead of 10136.0)
```

## Minimal reproducer
```c
long double add_to_param(long double pc, int val) {
  pc += val;    // BUG: has no effect
  return pc;
}
```
- `long double` param `+=` int → **broken** (returns original value)
- `long double` local `+=` int → works fine

## Root cause analysis (in progress)

### IR generated for the broken case
```
0000: PARAM0[call_0] P1           # convert val (int) to double
0001: CALL __aeabi_i2d --> T0
0002: PARAM0[call_1] P0           # add P0 + T0
0003: PARAM1[call_1] T0
0004: CALL __aeabi_dadd --> T1
0005: P0 <-- T1 [STORE]           # store result back to P0  ← BUG HERE
0006: T2 <-- P0 [LOAD]            # load P0 for return
0007: RETURNVALUE T2
```

After register allocation:
```
0005: R4(P0) <-- R0(T1) [STORE]   # only writes low word!
0006: R0(T2) <-- R4(P0) [LOAD]    # reads R4 (new low) + R5 (stale high)
```

### Disassembly confirms the bug
```asm
; Prologue: P0 (long double, 64-bit) saved to register pair
mov r4, r0    ; save P0 low word
mov r5, r1    ; save P0 high word

; ... __aeabi_i2d and __aeabi_dadd calls ...
; Result of dadd is in (r0, r1)

mov r4, r0    ; ← BUG: only stores low word to r4
              ;   r5 (high word) is NOT updated with r1!

; Return:
mov r0, r4    ; low word (correct - new value)
mov r1, r5    ; high word (WRONG - still original value!)
```

### Why it happens
The ASSIGN operation (`P0 <-- T1`) goes through `tcc_gen_machine_assign_op()` in [arm-thumb-gen.c](arm-thumb-gen.c#L6830). This function checks `irop_is_64bit(dest)` to decide whether to use the 64-bit assign path (`assign_op_64bit()`).

**Hypothesis**: The `btype` field on the P0 destination operand is not set to `IROP_BTYPE_FLOAT64` (value 3), so `irop_is_64bit()` returns false, and the code falls through to the simple 32-bit `mov` path.

### Debug instrumentation added
Temporary debug print added at [ir/codegen.c](ir/codegen.c) line ~1508 (TCCIR_OP_ASSIGN case) to verify the btype value at codegen time. **This needs to be built and tested.**

## Next steps

1. **Build with debug print** and run the test to confirm the btype value on the ASSIGN dest operand
2. **Trace where btype gets lost** — either:
   - The IR generation (`tccgen.c`) doesn't set btype when creating the ASSIGN to P0
   - The register allocation pass (`tccls.c`) or fill-registers pass strips/overwrites the btype
   - The operand encoding rounds trips incorrectly for parameter vregs
3. **Fix**: Ensure the `btype` is preserved as `IROP_BTYPE_FLOAT64` for `long double` parameter destinations in ASSIGN operations
4. **Verify** with the original test and the minimal reproducer
5. **Remove debug instrumentation**

## Key files
- [arm-thumb-gen.c](arm-thumb-gen.c#L6726-L6870) — `assign_op_64bit()` and `tcc_gen_machine_assign_op()`
- [tccir_operand.h](tccir_operand.h#L201) — `irop_is_64bit()` checks btype
- [ir/mat.c](ir/mat.c#L671) — `tcc_ir_materialize_dest_ir()` also checks `irop_is_64bit()`
- [ir/codegen.c](ir/codegen.c#L1508) — ASSIGN dispatch (debug print added here)
