# Phase 5: Simplify Stack and Spill Management

> **Status: 🔄 Partial** — committed `0e772abb phase 5: remove dead files and dead TCCStackSlot fields`. IROperand codegen-time flags and `tccir_operand.h` deduplication remain.

## Goal

With backend-driven materialization complete, clean up data structures that were only needed to support the old materialization layer.

## Changes

### 5.1: Simplify `IROperand`

**Remove fields that are only used for materialization state encoding:**

| Field | Current Use | Replacement |
|---|---|---|
| `pr0_spilled` | Set by `fill_registers_ir()` | `MachineOperand.kind == MACH_OP_SPILL` |
| `pr1_spilled` | Set by `fill_registers_ir()` | `MachineOperand.is_64bit && MACH_OP_SPILL` |
| `is_local` | Set by `fill_registers_ir()` | `MachineOperand.kind == MACH_OP_FRAME_ADDR` |
| `is_llocal` | Set by `fill_registers_ir()` | `MachineOperand.kind == MACH_OP_SPILL + needs_deref` |
| `is_param` | Set by `fill_registers_ir()` | `MachineOperand.kind == MACH_OP_PARAM_STACK` |

**Note:** These fields are set by `tcc_ir_fill_registers_ir()` which is deleted in Phase 4. After Phase 4, nothing writes to these fields. Removing them shrinks `IROperand` and eliminates the possibility of stale/incorrect flag state.

**Caution:** Verify that no IR-level pass (optimization, liveness) reads these fields. They should only be read during codegen.

### 5.2: Remove materialization result structs

Delete from `tccir.h` or `ir/mat.h`:

```c
// REMOVE:
typedef struct TCCMaterializedValue { ... };
typedef struct TCCMaterializedAddr { ... };
typedef struct TCCMaterializedDest { ... };
typedef struct TCCMatValue { ... };
typedef struct TCCMatAddr { ... };
typedef struct TCCMatDest { ... };
```

### 5.3: Simplify `TCCStackSlot`

**Remove fields that only existed for materialization decisions:**

| Field | Purpose | Needed? |
|---|---|---|
| `addressable` | Told materialization layer not to spill this | **Remove** — backend decides |
| `live_across_calls` | Told materialization to use callee-saved reg | **Remove** — allocator handles this |

Keep: `kind`, `vreg`, `offset`, `size`, `alignment` — these are fundamental to stack layout.

### 5.4: Remove VT_LLOCAL handling from backend

**Action:** Search `arm-thumb-gen.c` for `is_llocal` or `VT_LLOCAL` references. With `MachineOperand`, the double-indirection case is expressed as `MACH_OP_SPILL` with `needs_deref=true` — there's no separate code path.

### 5.5: Consolidate operand headers

**Current state:** There are two near-duplicate operand headers:
- `tccir_operand.h` (567 lines, 17-bit position)
- `ir/operand.h` (539 lines, 18-bit position)

**Action:** Eliminate the older `tccir_operand.h` and keep only `ir/operand.h`. Update all `#include "tccir_operand.h"` to `#include "ir/operand.h"`.

This is a maintenance hazard flagged during review — fixing it here prevents future bugs from edits to the wrong copy.

## Implementation Steps

### Step 5.1: Audit field usage

```bash
# Verify these fields are only read during codegen (now deleted):
grep -rn 'pr0_spilled\|pr1_spilled' --include='*.c' --include='*.h' | grep -v 'ir/mat.c\|ir/codegen.c'
grep -rn 'is_llocal' --include='*.c' --include='*.h' | grep -v 'ir/mat.c\|ir/codegen.c'
grep -rn 'is_local' --include='*.c' --include='*.h' | grep -v 'ir/mat.c\|ir/codegen.c'
```

Any unexpected callers need investigation before removal.

### Step 5.2: Remove fields from `IROperand`

Edit `ir/operand.h` to remove `pr0_spilled`, `pr1_spilled`, `is_local`, `is_llocal`, `is_param` bitfields.

**Note:** This changes `IROperand` layout. Since it's `__attribute__((packed))` at 10 bytes, removing 5 bits saves space and may improve cache behavior during IR passes.

### Step 5.3: Remove `TCCMaterializedValue`/`Addr`/`Dest` structs

Edit `tccir.h` to delete these struct definitions and any function declarations that reference them.

### Step 5.4: Simplify `TCCStackSlot`

Edit `tccir.h` or `ir/stack.h` to remove `addressable` and `live_across_calls` fields.

### Step 5.5: Consolidate operand headers

1. Diff `tccir_operand.h` vs `ir/operand.h` to identify differences
2. Ensure `ir/operand.h` is the superset
3. Replace all `#include "tccir_operand.h"` with `#include "ir/operand.h"`
4. Delete `tccir_operand.h`

### Step 5.6: Compile and test

```bash
make clean && make cross -j16
make test -j16
make test-gcc-torture-compile
```

## Expected Impact

| Metric | Change |
|---|---|
| `IROperand` size | 10 bytes → ~9 bytes (5 bits freed) |
| Struct types deleted | 6 (3 legacy + 3 new wrapper) |
| `TCCStackSlot` fields | 2 removed |
| Duplicate headers | Consolidated (`tccir_operand.h` deleted) |
| Dead code | All VT_LLOCAL-specific code paths removed |

## Current State (After `0e772abb`)

### Done
- Dead `TCCStackSlot` fields removed (`addressable`, `live_across_calls` — these were never set meaningfully after Phase 0)
- `ir/operand.c`, `ir/operand.h`, `ir/mat.c` deleted (Phase 4)

### Remaining: IROperand codegen-time flags

The following fields remain in `tccir_operand.h` because `tcc_ir_fill_registers_ir()` still sets them (the function is still called for all non-MOP instruction paths):

| Field | Who sets it | Who reads it | Blocked by |
|-------|------------|--------------|------------|
| `pr0_reg` / `pr1_reg` | `fill_registers_ir()` | `machine_op_from_ir()` case 4; direct reads in `arm-thumb-gen.c` | Phase 2 completion |
| `pr0_spilled` / `pr1_spilled` | `fill_registers_ir()` | `machine_op_from_ir()` via `IROP_TAG_STACKOFF` path | Phase 2 completion |
| `is_llocal` | `fill_registers_ir()` / IR opts | `machine_op_from_ir()` for `needs_deref`; `tccopt.c` | Phase 2 completion |
| `is_local` | IR construction + `fill_registers_ir()` | Many places in `arm-thumb-gen.c` and `tccopt.c` | Phase 2 completion |
| `is_param` | `fill_registers_ir()` | `arm-thumb-gen.c` call site handling | Phase 2 completion |

**Important:** `is_local` and `is_llocal` have a dual role — they are both IR-semantic (set during IR construction in `tccgen.c`) and codegen-time (read/mutated by `fill_registers_ir`). This dual role makes them harder to remove than the pure codegen-time fields.

### Remaining: `tccir_operand.h` deduplication

Two near-identical operand headers still exist:
- `tccir_operand.h` (root, 17-bit position encoding)
- `tccir_operand.c` (root, companion)

The `ir/` subdirectory no longer has `ir/operand.h` (deleted in Phase 4). The deduplication goal was to eliminate one copy, but since only `tccir_operand.h` remains, this is now moot — the duplication is gone. No further action needed on this item.

## Verification Checklist

- [x] Dead `TCCStackSlot` fields removed (`addressable`, `live_across_calls`)
- [x] `ir/mat.c`, `ir/operand.c`, `ir/operand.h` deleted
- [x] `make test -j16` passes
- [ ] `pr0_reg`, `pr0_spilled`, `is_llocal` removed from `tccir_operand.h` (blocked until Phase 2 complete + `fill_registers_ir` deleted)
- [ ] `is_local` and `is_param` codegen-time semantics separated from IR semantics (complex — needs careful audit of `tccopt.c` and `tccgen.c` usage)
- [ ] `tcc_ir_fill_registers_ir()` deleted from `ir/codegen.c` (blocked until Phase 2 complete)
- [ ] `make test-gcc-torture-compile` passes after field removal
