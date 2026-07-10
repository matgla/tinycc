# Plan: next SSA bitfield-fusion wins

**Status:** planned · **Branch:** `legacyOptRemoval` · Follow-on to the
`const_prop`→SSA work in [`plan_legacy_flat_ir_ssa_retire.md`](plan_legacy_flat_ir_ssa_retire.md).

Context: `ssa:narrow` now fuses `(x>>n)&((1<<w)-1)` and `SHR`-fed `UBFX(#0,#w)`
into `UBFX(x,#n,#w)` for any width (ir/opt/ssa_opt_narrow.c, `gen_and_fold` /
`gen_ubfx_fold`), and `ssa:branch` Case 3 folds reflexive bitfield self-compares
(`ssa_cmp_extract_desc`). Two extensions remain. Method for both: A/B with
`TCC_DISABLE_PASS=const_prop` + `arm-none-eabi-objdump` per-function size diff;
gate on `make test` + `compare_worktree.py --baseline-commit HEAD --opt o2`.

Encodings (verified): `UBFX` param = `lsb | (width<<5)`; `(x<<a)>>b` (logical) =
`UBFX(x, b-a, 32-b)`; ARM CMP barrel shifts **src2 only**, byte = `bs & 31`,
type = `(bs>>5)&7` (1 LSL, 2 LSR, 3 ASR, 4 ROR). SSA-gen fuse convention:
`ssa_opt_add_use_instr(newsrc)`, set fused-away dest `vi->use_count = 0`,
`ssa_opt_nop_instr(def)` — see arch/arm/ssa_opt_arm.c and the two narrow gens.

## 1. Signed bitfield extract → SBFX  ✅ LANDED

**Goal:** the signed analog of the UBFX fusion. `(x << a) >> b` with an
**arithmetic** right shift is a signed field read that ARM does in one
`SBFX Rd, Rn, #lsb, #width` — previously emitted as a `lsls`+`asrs` pair.

**What shipped (post-RA, mirrors `shift_pair_to_ubfx`):**
- `TCCIR_OP_SBFX` added to `TccIrOp` (tccir.h) + `irop_config` `{1,1,1}`
  (`ir/gen/config.c`) + dump name (`ir/dump.c`) + value-read whitelist
  (`ir/opt_memory.c`, harmless — pass runs pre-RA so never sees it).
- Backend: `tcc_gen_machine_sbfx_mop` in `arm-thumb-gen.c` (clone of
  `ubfx_mop`, base opcode `0xF3400000` vs `0xF3C00000`; same imm3/imm2/widthm1
  layout — confirmed against `th_sbfx`/`TH_SBFX` in `thop_bitfield.c`), dispatch
  in `ir/codegen.c`, prototype in `tcc.h`.
- Fusion: `tcc_ir_opt_shift_pair_to_ubfx` (`ir/opt_fusion.c`) now also matches an
  **`SAR`** outer shift (`is_signed`) and emits `TCCIR_OP_SBFX`. `(x<<a)>>b`
  arithmetic, `1<=a<=b<=31`, single-use SHL, non-lval reg source, same block →
  `SBFX(x, b-a, 32-b)`. Runs at the same point as the UBFX case, after
  `tcc_ir_barrel_shift_fusion` (so only pairs that did NOT fold into a
  consuming barrel survive to be matched — the strict-win cases).

**Verified:** 13497 IR tests pass; QEMU runtime check (`sbfxrt.c`, widths 1/5/8/13,
edge values incl. `INT_MIN`/`0x7fffffff`) all correct; `e1..e5`+`store_field`
emit `sbfx`.

**Caveat on payoff:** SBFX is a **cycle** win (2 shifts → 1 extract) but often
**size-neutral**: a 16-bit-encodable `lsls`+`asrs` pair is 4 bytes, exactly like
the 32-bit `sbfx`. Net size shrinks **only** when the pair would need 32-bit
encodings (high regs r8–r12, IT-block/flag constraints). So under the pure-size
`compare_worktree` metric the win is modest and register-pressure-dependent.

### 1a. Follow-up: fuse the lval-deref source (`*P` feeding the SHL)  — OPTIONAL

The most common signed-field-read shape does **not** fuse today. For
`sink = p->field;` the load folds into the SHL as a deref source:

```
R1(T1) <-- R0(P0)***DEREF*** SHL #27      -> ldr r2,[r0]; lsls r1,r2,#27
R0(T2) <-- R1(T1)            SAR #27       -> asrs r0,r1,#27
GlobalSym***DEREF*** <-- R0(T2) [STORE]
```

Both the UBFX and SBFX passes bail here on the `t0.is_lval` guard, so it stays a
3-instruction `ldr; lsls; asrs`. It **could** become `ldr; sbfx` (the backend
already materializes the deref into a scratch via `mach_ensure_in_reg`, which
`sbfx_mop` calls, so an SBFX carrying a deref src1 would emit the same `ldr`
followed by one `sbfx`).

**Why it's parked (not obviously worth it):**
- **Size-neutral** in the common 16-bit case: `ldr;lsls;asrs` = 2+2+2 = 6 B vs
  `ldr;sbfx` = 2+4 = 6 B. Only a cycle win (3→2). Same trade-off as §1's caveat.
- **New correctness burden:** dropping the `is_lval` guard means the memory read
  moves from the SHL's position to the outer-shift's position. The current guard
  only checks *vreg* redefinition between the two shifts; a deref source also
  needs **no aliasing `STORE`/`STORE_INDEXED`/`STORE_POSTINC`/call** in that
  window (they're adjacent in the straight-line case, but the guard must enforce
  it, not assume it). Getting this wrong is a silent miscompile.
- Applies symmetrically to the existing UBFX pass — do both or neither.

**If pursued:** in `tcc_ir_opt_shift_pair_to_ubfx`, allow `t0.is_lval` when (a)
the SHL src is a plain deref (not indexed/postinc), (b) add the memory-write
barrier scan to the existing same-block loop, and (c) pass the deref operand
straight through to `set_src1` so the mop materializes it. Guard against the
64-bit and LOAD_INDEXED forms. Low priority: cycle-only win, real risk.

## 2. Case 3 bitfield-CMP — cross-reload bases

**Goal:** `ssa:branch` Case 3 currently folds `CMP a,b` only when both operands
extract the same field of the **same base vreg** (`b1 == b2` in
`ssa_fold_cmp_jumpif`). In the 20040709 struct-copy pattern, fields `j`/`k`
compare `x.f` (from loaded `T0`) against `s.f` (a fresh `GlobalSym` **reload**) —
different bases, so Case 3 misses them. The load_cse cascade catches some once
the abort branch dies, but not across the intervening calls.

**Approach:** in `ssa_cmp_extract_desc`, treat two bases as equal when they are
provably the same value:
- `T0 = LOAD(sym)` and the other operand's base is a `LOAD(sym)` of the **same**
  `(sym, addend)` with **no store/call to that sym between** the two loads.
- Reuse the alias reasoning already in ir/opt_alias.c / the load_cse kill rules;
  do NOT hand-roll aliasing.

**Steps:**
1. In `ssa_cmp_extract_desc`, when the operand's ultimate base is an lval
   `LOAD`/deref of a symref, return a canonical `(sym,addend)` base id instead
   of the vreg; match those in Case 3.
2. Guard: bail if any `STORE`/`STORE_INDEXED`/call lies between the two loads
   (conservative — same-block first, extend to dominance later).
3. Test: 20040709-1 `test*` fns (fields j/k should now fold like i); confirm no
   over-fold where the global is legitimately mutated between reads (add a
   volatile / stored-between negative test).

**Risk:** medium — aliasing correctness. Keep it same-block + no-intervening-
write first. **Payoff:** the rest of the 20040709 cluster + similar
struct-copy-then-compare code (memcmp-free equality checks).

## Recommended order

Do **#2 first** (no new opcode, reuses existing alias/load-cse machinery, direct
follow-on to a landed pass) then **#1** if the signed-extract gap shows up in the
metrics as material.
