# TCC ARMv8-M Optimization Plan

## Current State (2026-04-20)

### Benchmark: mibench_rijndael (AES)

| Function | TCC -O2 | GCC -O2 | Ratio | Notes |
|----------|---------|---------|-------|-------|
| bench_mibench_rijndael | 74 | 64 | 1.15x | Nearly matched |
| decrypt | 1211 | 981 | 1.23x | Includes GCC .part.0 |
| encrypt | 1213 | 989 | 1.22x | Includes GCC .part.0 |
| main | 8 | 8 | 1.00x | Matched |
| set_key | 584 | 282 | 2.07x | Largest ratio |
| **TOTAL** | **3090** | **2324** | **1.32x** | |

### Key Bottleneck Analysis (2026-04-20)

**513 stack loads** in TCC output vs almost none in GCC for the same values.
**283 literal pool loads** for table base addresses GCC keeps in registers.
**0 strd/ldrd** instructions emitted by TCC vs 13 in GCC's set_key alone.

### Completed Optimizations

| # | Optimization | Location | Impact |
|---|-------------|----------|--------|
| 1 | SL-FWD LVAL forwarding | ir/opt.c | Constants propagate through struct field reads |
| 2 | ALU forward LEA extension | ir/opt.c | CMP resolves through TEMP pointer offsets |
| 3 | Iterative const_prop in SL-FWD loop | tccgen.c | Multi-level addition chains fold in one SL-FWD iteration |
| 4 | LOAD_INDEXED forwarding | ir/opt.c | Constants forward through indexed memory loads |
| 5 | VAR hash collision fix | ir/opt.c | Sentinel sym separates VAR/StackLoc namespaces in SL-FWD |
| 6 | compact_nops in SL-FWD loop | tccgen.c | Removes phantom BB boundaries between iterations |
| 7 | Soft-float constant folding | ir/opt.c | Folds `__aeabi_fadd/fsub/fmul/fdiv/f2iz` with constant args |
| 8 | Struct-param inlining | tccgen.c | Static functions with small struct params get inlined |
| 9 | Struct copy via vstore | tccgen.c | Proper memcpy for struct params during inline expansion |
| 10 | Byte-cast folding (SHL+SHR→AND) | ir/opt.c | Eliminates redundant shift pairs for `(byte)x` casts |
| 11 | UBFX IR opcode + backend | tccir.h, ir/opt.c, ir/codegen.c, arm-thumb-gen.c | Fuses SHR+AND→UBFX for register sources |
| 12 | GlobalSym CSE (partial) | ir/opt.c | Hoists repeated symbol addresses where NOP slots exist |

### Tests that reached GCC parity

- `test_struct_return.c`: 232 → **8** instructions (was 29x, now 1.0x)
- `test_struct_pass_by_value.c`: 150 → **8** instructions (was 18.75x, now 1.0x)
- `73_float_ops.c`: constant-folded to match GCC
- `mibench_rijndael.c main()`: 8 = 8 (1.0x)

---

## Remaining Optimization Targets

### Priority 1: Fix LICM (Loop-Invariant Code Motion)

**Impact**: ~300+ fewer instructions in encrypt/decrypt, ~100 in set_key
**Status**: LICM is disabled (`#if 0` in tccgen.c) due to three bugs

This is the **single highest-impact optimization**. The AES inner loops reload 4 table
base pointers from the stack on every round (8 loads/round × 10 rounds = 80 wasted loads
per function). LICM would hoist these into callee-saved registers before the loop.

#### Bug 1: Spurious loops from switch-case back-edges
- [ ] **Fix** `ir/licm.c` `tcc_ir_detect_loops()`: add dominance check for back-edges
- **Location**: `ir/licm.c`, `tcc_ir_detect_loops()`
- **Symptom**: Loop detector treats switch-case `break` jumps (which jump back to the outer while-loop header) as loop back-edges, creating fake "loops" from individual case bodies.
- **Effect**: `hoist_const_exprs_from_loop` hoists expressions from one switch case into another case's basic block.
- **Test**: `bug_ternary_switch.c` — test4 returns 4 instead of 12.
- **Fix approach**: The loop detector uses back-edge detection (JMP to earlier instruction). Need to add a dominance check: a back-edge is only a real loop if the target dominates the source. Switch dispatch targets don't dominate individual case bodies.
- **Alternative**: Only detect loops where the back-edge is a conditional jump (JUMPIF), not an unconditional JMP. Switch `break` statements generate unconditional JMPs.

#### Bug 2: Pure-call hoisting cascading with multi-def vregs
- [ ] **Fix** `ir/licm.c` `is_operand_loop_invariant_ex()`: verify single-definition for hoisted vregs
- **Location**: `ir/licm.c`, `tcc_ir_hoist_pure_calls()` + `is_operand_loop_invariant_ex()`
- **Symptom**: After hoisting `func_a(100)`, its result vreg is marked "hoisted" (invariant). Then `func_b(result)` is checked — `result` is in the hoisted list, so it's considered invariant. But `result` is ALSO written by `func_b(result)`, `func_c(result)`, etc. — it has multiple definitions.
- **Effect**: Entire call chains get hoisted, producing wrong results.
- **Test**: `bench_control.c` — `function_calls` returns 0 instead of 91967.
- **Fix approach**: In `is_operand_loop_invariant_ex`, when a vreg is in the hoisted list, also verify it has exactly ONE definition in the loop body. If it has multiple definitions, it's NOT invariant despite being hoisted.

#### Bug 3: Stack address hoisting with conditional stores
- [ ] **Fix** `ir/licm.c` `hoist_from_loop()`: skip stack addr hoisting with conditional stores
- **Location**: `ir/licm.c`, `hoist_from_loop()`
- **Symptom**: Hoisting `Addr[StackLoc[arr]]` from a loop that has conditional `arr[i] = val` stores can cause the array index computation to use wrong values.
- **Test**: `pr94734.c` — `bar(25, 0x25, 0xdeadbeefbeefdead, 4)` should return 42 but doesn't.
- **Fix approach**: Skip stack address hoisting when the loop body contains conditional stores to the same stack region. Or: only hoist addresses that are used in LOAD operations (reads), not in STORE operations (writes).

#### LICM verification after fixes
- [ ] Re-enable LICM and verify all three test cases pass
- [ ] Verify with `-dump-ir` that table bases appear before the loop, not inside it
- [ ] Run `make test -j16` for regression check
- [ ] Measure mibench_rijndael improvement

#### What LICM would enable for AES
Once fixed, LICM would hoist the 4 AES table base addresses (`GlobalSym(2374)+0/1024/2048/3072`) out of the encryption loop. Each address is currently loaded from the literal pool on every round (~10 rounds × 4 addresses × 4 words = 160 loads). Hoisting saves ~156 literal pool loads per encrypt/decrypt function.

Additionally, LICM would enable the GlobalSym CSE to work properly (by placing the base addresses in registers before the loop), which further reduces register pressure and spill traffic.

### Priority 2: Redundant AND elimination (quick win)

**Impact**: ~16-26 instructions saved
**Difficulty**: Easy

- [ ] Add rule in `ir/opt.c`: `AND(AND(x, mask), mask) → AND(x, mask)`
- [ ] More generally: if the input to AND is already narrower than the mask, eliminate the AND

The IR at bench_mibench_rijndael line 6311-6312 shows:
```
R3(T12) <-- R2(T11) AND #255
R2(T14) <-- R3(T12) AND #255    ← redundant
```
Codegen emits both `and.w` instructions. Also detect `SHL #24; SHR #24` applied to
values already known to be 8-bit (result of UBFX or AND #255) and fold them.

**Files**: `ir/opt.c`

### Priority 3: LOAD_INDEXED with GlobalSym base

**Impact**: ~200 fewer instructions in encrypt/decrypt
**Dependency**: Partially benefits from LICM (Priority 1), but can work standalone

**Pattern**:
```
AND Rn, #255           ; extract byte
SHL Rn, #2             ; multiply by 4
ADD Rn, GlobalSym      ; table base + index
LDR Rd, [Rn]           ; load from table
```

**Target** (ARM Thumb-2):
```
UBFX Rn, Rx, #N, #8   ; extract byte
LDR Rd, [table_reg, Rn, LSL #2]  ; indexed load
```

- [ ] Extend `tcc_ir_opt_disp_fusion` to recognize `SHL #2 + ADD GlobalSym + DEREF` as `LOAD_INDEXED` with symbol base
- [ ] Backend emits `LDR Rd, [base_reg, Rn, LSL #2]` where `base_reg` holds table address

**Files**: `ir/opt.c` (disp_fusion), `arm-thumb-gen.c`

### Priority 4: UBFX for DEREF+SHR patterns

**Impact**: ~100 fewer instructions in encrypt/decrypt

- [ ] Split fused DEREF+SHR into separate LOAD + SHR to enable UBFX fusion

**Current state**: The UBFX fusion (SHR+AND→UBFX) only fires when the SHR source is a register. In AES, the SHR source is a DEREF (memory load fused with the shift): `T <-- StackLoc[X] SHR #8`.

```
BEFORE: T1 <-- StackLoc[X] SHR #8    ; fused load+shift
        T2 <-- T1 AND #255

AFTER:  T0 <-- StackLoc[X] [LOAD]    ; separate load
        T2 <-- T0 UBFX #(8|8<<5)     ; extract byte (ubfx)
```

**Files**: `ir/opt.c`

### Priority 5: Paired load/store (`strd` / `ldrd`)

**Impact**: ~60 instructions saved (40 in set_key, 20 in encrypt/decrypt)
**Difficulty**: Medium

GCC emits 13 `strd`/`ldrd` in set_key alone. TCC emits zero.

- [ ] Add peephole pass (post-regalloc or during codegen) to detect:
  - Two consecutive `STR Rx, [Rbase, #N]` + `STR Ry, [Rbase, #N+4]` → `STRD Rx, Ry, [Rbase, #N]`
  - Two consecutive `LDR Rx, [Rbase, #N]` + `LDR Ry, [Rbase, #N+4]` → `LDRD Rx, Ry, [Rbase, #N]`
- [ ] Constraints: offset must be word-aligned, Rx != Ry, registers must not conflict
- [ ] Thumb-2 encoding: `STRD` is T1 (32-bit), offset range `[-1020, +1020]` in steps of 4
- [ ] Add opcode builder in `arm-thumb-opcodes.c` for `STRD`/`LDRD` if not already present

**Files**: `arm-thumb-gen.c`, `arm-thumb-opcodes.c`, `ir/codegen.c`

### Priority 6: Reduce literal pool usage

**Impact**: ~30 instructions + eliminates branch-around-pool overhead
**Dependency**: Benefits greatly from LICM (Priority 1)

TCC uses 283 literal pool loads (`ldr Rx, [pc, #N]`) in the benchmark. When a global
base is already in a register, emit `add Rx, Rbase, #offset` instead of a new pool entry.

For AES tables: `Te0 = base`, `Te1 = base+1024`, `Te2 = base+2048`, `Te3 = base+3072`
can all be computed from one base register with shifted add.

- [ ] When `GlobalSym + constant` and the base `GlobalSym` is already in a register, use `ADD` not literal pool
- [ ] Count and eliminate unnecessary literal pool entries
- [ ] Reduces branch-around-pool overhead (visible in dump_ir.txt `literal_pool` warnings)

**Files**: `arm-thumb-gen.c`, `ir/mat.c`

### Priority 7: Register pressure reduction in set_key

**Impact**: ~100 instructions saved in set_key (584→~480)
**Difficulty**: Medium

The round-key pointer (`[sp, #20]`) is loaded from stack ~40 times in the loop body.

- [ ] Trace why this pointer gets spilled — likely too many live values competing for r0-r12
- [ ] Consider if some temporaries can have shorter live ranges to free up a register
- [ ] Investigate regalloc spill weight: values with high use-count in loops should prefer callee-saved regs

**Files**: `tccls.c`, `ir/live.c`

### Priority 8: Backend peepholes

**Impact**: ~50-100 instructions across all functions

- [ ] **UXTB for AND #255**: Use narrower Thumb-1 `UXTB Rd, Rn` (2 bytes) instead of wide `AND.W` (4 bytes)
- [ ] **Redundant MOV elimination**: Some remain after existing peepholes
- [ ] **LDR/STR with pre-indexed addressing**: For `ADD Rn, #offset; LDR Rd, [Rn]`, use `LDR Rd, [Rn, #offset]`

**Files**: `arm-thumb-gen.c`

### Priority 9: Structural optimizations (aspirational)

#### Function outlining / splitting
GCC splits encrypt/decrypt into thin wrappers + `.part.0` (8 insns for early exit vs
full prologue). Low priority since it saves few instructions overall.

- [ ] Detect functions where first BB is a condition check leading to early return
- [ ] Outline the heavy path into a separate function

#### Strength reduction for multiply-by-constant
In bench_mibench_rijndael, TCC emits `mul.w r2, r0, r3` for `i * 17`. GCC uses
`rsb r3, fp, fp, lsl #5` tricks.

- [ ] Replace `MUL x, #N` with shift+add sequences when N has few set bits
- [ ] Common: `*3`=`add x,x,x,lsl#1`, `*9`=`add x,x,x,lsl#3`, `*17`=`add x,x,x,lsl#4`

---

## Expected Results

| After | Target ratio | Est. instructions | Notes |
|-------|-------------|-------------------|-------|
| Current | 1.32x | 3090 | |
| P1 (LICM) | **1.15x** | ~2670 | Biggest single win |
| P2 (AND elim) | 1.14x | ~2650 | Quick win |
| P3 (LOAD_INDEXED) | 1.10x | ~2550 | Synergy with P1 |
| P4 (UBFX) | 1.08x | ~2500 | |
| P5 (strd/ldrd) | 1.06x | ~2440 | |
| P6 (literal pool) | 1.05x | ~2410 | |
| P7 (regalloc) | 1.03x | ~2390 | |
| P8-P9 (peepholes) | **~1.02x** | ~2370 | Near GCC parity |

## Verification

After each change:
```bash
# Run the benchmark
cd tests/ir_tests && python run.py -c mibench_rijndael.c --cflags="-O2"

# Dump IR to check optimization effects
cd tests/ir_tests && python run.py -c mibench_rijndael.c --cflags="-O2" --dump-ir

# Run full test suite for regressions
make test -j16
```

---

## Architecture Notes

### Optimization Pipeline (tccgen.c)
```
Iterative loop (max 10 iterations):
  1. DCE
  2. const_prop + global_init_prop + const_prop_tmp + const_string_calls
  3. branch_folding + setif_branch_fuse + stack_bool_diamond
  4. var_tmp_fwd + var_to_tmp
  5. value_tracking (includes soft-float folding)
  6. nonneg_branch_fold + float_branch_fold + VRP
  7. float_narrowing
  8. copy_prop
  9. cse_arith

GlobalSym CSE (before compact_nops)
compact_nops
Global CSE
Jump threading + fallthrough elimination

Fusion passes:
  stack_addr_cse → MLA+indexed fusion → disp_fusion → LEA fold
  postinc_fusion → bool passes → return optimization

compact_nops

SL-FWD loop (max 12 iterations):
  sl_forward → iterative const_prop (4 rounds) →
  branch_folding → DCE → jump_threading → eliminate_fallthrough →
  compact_nops

Post-SL_FWD cleanup
DSE + dead_addrvar_elim + redundant_var_assign

LICM (disabled) → IV strength reduction → strength reduction
Late copy_prop + DSE
Loop postinc fusion
```

### Key Data Structures
- **SL-FWD hash table**: 128-bucket hash on `(Sym*, offset)` for store-load forwarding
- **LEA map**: Maps TEMP positions to resolved stack offsets for pointer-through-LEA resolution
- **fwd_tmp tracking**: Tracks TEMP values forwarded by SL-FWD for transitive resolution
- **VT state (value_tracking)**: Tracks constant values of VAR vregs with generation counters
