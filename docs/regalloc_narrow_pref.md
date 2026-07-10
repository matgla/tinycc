# Narrow-encoding register preference (`ra:narrow_pref` + scratch picker)

Thumb-2 16-bit encodings mostly require r0-r7; the linear scan's default
non-call-crossing order (`{0,1,2,3,12,4..11}`) parked values in r12 where every
ALU/load/store use pays the 32-bit encoding, and the spilled-operand scratch
picker preferred ip/lr over prolog-pushed dead low regs. Two changes:
the allocator heuristic (knob: `TCC_DISABLE_PASS=ra:narrow_pref`,
byte-identical corpus when disabled) and a width-aware scratch preference.
Result on the ir_tests -O2 corpus: allocator alone net -1324 bytes .text;
with the scratch preference net **-4940** (163 objects smaller / 2 larger,
both +4/+8 allocation-shift noise).

## Mechanism

1. `RegAllocTarget.op_narrow_capable(op, src2_is_imm, scale)` — arch classifier
   (arch/arm/arm_regalloc.c) marking IR ops whose lowering has a T16 form when
   operands land low. Notable exclusions: reg-reg ASSIGN/CMP (T16 hi forms
   exist), indexed LDR/STR with a nonzero shift (T32-only), DIV/MLA/64-bit/FP.
2. `ra_build_narrow_weights` (ir/regalloc.c) — one IR walk counting, per
   interval, STATIC references from narrow-capable ops (`narrow_uses`).
   Static, not loop-weighted: code size is per static instruction.
   `ra_coalesce_graph` sums the counts into class representatives.
3. `ra_linear_scan` single-INT non-crossing order becomes
   `{0,1,2,3,4,5,6,7,12,8..11}` when the interval is narrow-DENSE.

## Guards (each empirically necessary)

- **Density gate** (`narrow_uses * 8 >= end - start`): a long-lived interval
  with sparse narrow uses must not hog a low reg. rijndael encrypt: the
  round-key base grabbing r4 starved ~25 short load/eor temps into ip,
  +88 bytes; with the gate rijndael is -4. Factor sweep: 4 → net -1252,
  8 → -1324, 16 → -1276.
- **Fresh-push gate**: take a not-yet-dirty r4-r7 over r12 only when the push
  list already exists (`has_call` ⇒ `push {lr}`, growing it is free) or
  `narrow_uses >= 2` covers the 4-byte push/pop cost.
- **Pass-0 high-reg guard**: when the fresh-push gate skips r4-r7 and r12 is
  busy, do NOT fall through to fresh r8-r11 (wide `stmdb/ldmia` push/pop plus
  wide uses — strictly worse than fresh r4-r7); a second pass lifts the gate
  so gated-out low regs still beat spilling. Without this, 110's loop temp
  landed in r9: +24 vs the gated low reg.

## Scratch-picker width preference (arm-thumb-gen.c)

`tcc_ls_find_free_scratch_reg` picks r0-r3, then ip, then lr — so with r0-r3
busy every spill reload materialized through ip as `ldr.w ip, [sp/pc, #x]`
even when a prolog-pushed r4-r7 was dead at that instruction (the existing
pre-pushed fallback only ran when NOTHING was free). Now, when the picker
returns ip/lr, `get_scratch_reg_with_save` swaps in a pushed-and-dead r4-r7
(`scratch_pushed_dead_reg`, same liveness + reserved-reg conditions as the
old fallback, real-run only — dry-run still sizes for ip, so real-run can
only shrink). This alone was worth -3.6 KB on the corpus — spill-heavy
functions (fuzz monsters, 244/311/252) saved 100-170 bytes each.
