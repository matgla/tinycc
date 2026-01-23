# Plan: Remove pr0_reserved/pr1_reserved from SValue

## Overview

Remove the 2-bit reserved padding fields from the SValue bitfield layout and change `PREG_SPILLED` from `0x80` to `0x20`.

## Current Layout (8 bits per pr0/pr1)

```c
struct {
  uint8_t pr0_reg : 5;      // bits 0-4 (register 0-15 or 31=NONE)
  uint8_t pr0_reserved : 2; // bits 5-6 (unused padding)
  uint8_t pr0_spilled : 1;  // bit 7 (0x80)
};
```

## New Layout (6 bits per pr0/pr1)

```c
struct {
  uint8_t pr0_reg : 5;     // bits 0-4 (register 0-15 or 31=NONE)
  uint8_t pr0_spilled : 1; // bit 5 (0x20)
};
```

## Files to Modify

### 1. tccir.h
- [ ] Change `PREG_SPILLED` from `0x80` to `0x20`
- [ ] Add comment explaining bit layout

### 2. svalue.h
- [ ] Remove `pr0_reserved : 2` field
- [ ] Remove `pr1_reserved : 2` field
- [ ] Update comments

### 3. svalue.c
- [ ] Remove `sv->pr0_reserved = 0;` from `svalue_init()`
- [ ] Remove `sv->pr1_reserved = 0;` from `svalue_init()`

### 4. tccgen.c
- [ ] Remove `vtop->pr0_reserved = 0;` from `vsetc()`
- [ ] Remove `vtop->pr1_reserved = 0;` from `vsetc()`

### 5. tccir.c
Search and remove all `pr0_reserved = 0` and `pr1_reserved = 0` assignments:
- [ ] Line ~1713: `dest->pr0_reserved = 0;`
- [ ] Line ~1716: `dest->pr1_reserved = 0;`
- [ ] Line ~1729: `src1->pr0_reserved = 0;`
- [ ] Line ~1732: `src1->pr1_reserved = 0;`
- [ ] Line ~1745: `src2->pr0_reserved = 0;`
- [ ] Line ~1748: `src2->pr1_reserved = 0;`
- [ ] Line ~3095: `sv->pr0_reserved = 0;`
- [ ] Line ~3098: `sv->pr1_reserved = 0;`
- [ ] Line ~3186: `sv->pr0_reserved = 0;`
- [ ] Line ~3193: `sv->pr1_reserved = 0;`
- [ ] Line ~3272: `sv->pr0_reserved = 0;`
- [ ] Line ~3279: `sv->pr1_reserved = 0;`
- [ ] Line ~3356: `sv->pr0_reserved = 0;`
- [ ] Line ~3359: `sv->pr1_reserved = 0;`
- [ ] Line ~3368: `sv->pr0_reserved = 0;`
- [ ] Line ~3371: `sv->pr1_reserved = 0;`
- [ ] Line ~3415: `dest->pr0_reserved = 0;`
- [ ] Line ~3422: `dest->pr1_reserved = 0;`
- [ ] Line ~3446: `dest->pr0_reserved = 0;`
- [ ] Line ~3448: `dest->pr1_reserved = 0;`
- [ ] Line ~3462: `sv->pr0_reserved = 0;`
- [ ] Line ~3464: `sv->pr1_reserved = 0;`
- [ ] Line ~3479: `sv->pr0_reserved = 0;`
- [ ] Line ~3481: `sv->pr1_reserved = 0;`
- [ ] Line ~3731: `sv->pr0_reserved = 0;`
- [ ] Line ~3734: `sv->pr1_reserved = 0;`
- [ ] Line ~3754: `sv->pr0_reserved = 0;`
- [ ] Line ~3757: `sv->pr1_reserved = 0;`
- [ ] Line ~3784: `sv->pr0_reserved = 0;`
- [ ] Line ~3786: `sv->pr1_reserved = 0;`
- [ ] Line ~6894: `dest->pr0_reserved = 0;`
- [ ] Line ~6898: `dest->pr1_reserved = 0;`
- [ ] Line ~6949: `dest->pr0_reserved = 0;`
- [ ] Line ~6953: `dest->pr1_reserved = 0;`

### 6. arm-thumb-gen.c
- [ ] Line ~1913: `v1.pr0_reserved = 0;`
- [ ] Line ~2984: Remove if present

## Testing

After changes:
1. `make clean && make -j8`
2. Run the failing tests that were fixed:
   - `python run.py -c test_ge_operator.c`
   - `python run.py -c ../tests2/33_ternary_op.c`
   - `python run.py -c ../tests2/90_struct-init.c`
   - `python run.py -c ../tests2/93_integer_promotion.c`
3. Run full test suite

## Notes

- The packed format `(spilled ? PREG_SPILLED : 0) | reg` will still work
- `IRLiveInterval.allocation.r0/r1` stores packed values - unchanged semantics
- `thumb_gen_state.cached_global_reg` comparisons - unchanged semantics
- `PREG_NONE = 0x1F` and `PREG_REG_NONE = 0x1F` remain unchanged

## Verification Commands

```bash
# Find all pr0_reserved/pr1_reserved usages
grep -rn "pr0_reserved\|pr1_reserved" *.c *.h

# Find all PREG_SPILLED usages (verify they use the constant, not 0x80)
grep -rn "PREG_SPILLED\|0x80" *.c *.h | grep -v "PREG_SPILLED"
```
