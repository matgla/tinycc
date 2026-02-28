# Phase 1: New Operand Representation — `MachineOperand`

> **Status: 🔄 Partial** — `MachineOperand` type and `machine_op_from_ir()` fully implemented. Used on the MOP dispatch path for data-processing ops (Phase 2). `fill_registers_ir` still runs as prerequisite before `machine_op_from_ir` is called. Full removal of `fill_registers_ir` is blocked pending replication of its semantic transforms (see §Remaining Work).

## Goal

Replace the overloaded `IROperand` flags with a clear machine-level operand type that the backend can interpret without ambiguity. This separates "what the IR says" from "how the backend should materialize it."

## Current State

`IROperand` (defined in `ir/operand.h`, 10 bytes packed) encodes materialization state via a fragile combination of bit flags:

| Flag | Meaning | Set By |
|---|---|---|
| `pr0_reg` / `pr1_reg` | Physical register assignment | `tcc_ir_fill_registers_ir()` |
| `pr0_spilled` / `pr1_spilled` | Value is spilled to stack | `tcc_ir_fill_registers_ir()` |
| `is_local` | Stack-relative (frame offset in payload) | `tcc_ir_fill_registers_ir()` |
| `is_llocal` | Double indirection (spilled pointer) | `tcc_ir_fill_registers_ir()` |
| `is_lval` | Needs load through address | `tcc_ir_fill_registers_ir()` |
| `is_param` | Stack-passed function parameter | `tcc_ir_fill_registers_ir()` |
| `is_const` | Immediate constant | IR construction |
| `tag` | IROP_TAG_VREG/IMM32/STACKOFF/etc. | IR construction |

The backend (`arm-thumb-gen.c`) must test combinations of these flags to determine what to do:
- `pr0_spilled && !is_llocal` → load from spill slot
- `is_llocal` → load pointer from spill, then dereference
- `is_local && is_lval` → load from frame address
- `is_param && pr0_spilled` → load from parameter area

These combinations are error-prone and the source of most materialization bugs.

## Design

### `MachineOperand` type

```c
/* ir/machine_op.h */

typedef enum {
    MACH_OP_REG,          /* Value in physical register(s) */
    MACH_OP_SPILL,        /* Value in spill slot, needs load */
    MACH_OP_IMM,          /* Immediate constant */
    MACH_OP_FRAME_ADDR,   /* Address = FP + offset (address-of local) */
    MACH_OP_SYMBOL,       /* Symbol reference (global/extern) */
    MACH_OP_PARAM_STACK,  /* Stack-passed parameter in caller frame */
} MachineOperandKind;

typedef struct {
    MachineOperandKind kind;
    CType type;
    union {
        struct { int r0, r1; }           reg;    /* MACH_OP_REG */
        struct { int offset; int size; } spill;  /* MACH_OP_SPILL */
        struct { int64_t val; }          imm;    /* MACH_OP_IMM */
        struct { int offset; }           frame;  /* MACH_OP_FRAME_ADDR */
        struct { Sym *sym; int addend; } sym;    /* MACH_OP_SYMBOL */
        struct { int offset; int size; } param;  /* MACH_OP_PARAM_STACK */
    } u;
    int vreg;              /* Original vreg (for debug/liveness queries) */
    bool needs_deref;      /* Load through this address (replaces VT_LVAL) */
    bool is_64bit;         /* Two-register value */
} MachineOperand;
```

### Conversion function

```c
/* Replaces tcc_ir_fill_registers_ir() — instead of rewriting IROperand in
 * place with flag mutations, produce a clean MachineOperand. */
MachineOperand machine_op_from_ir(TCCIRState *ir, const IROperand *op);
```

This single function encapsulates the entire `tcc_ir_fill_registers_ir()` logic in a pure, side-effect-free mapping. It reads the register allocation results and the operand's IR-level tags to produce one of 6 unambiguous enum variants.

## Implementation Steps

### Step 1.1: Create `ir/machine_op.h`

**Action:** Create the header with the `MachineOperand` type, `MachineOperandKind` enum, and the `machine_op_from_ir()` declaration.

**Design decisions:**
- Keep it a plain C header (no C++ features)
- Include `tccir.h` for `IROperand`, `TCCIRState`
- `CType` comes from `tcc.h` — need a forward declaration or include

### Step 1.2: Implement `machine_op_from_ir()` in `ir/machine_op.c`

**Action:** Port the logic from `tcc_ir_fill_registers_ir()` (ir/codegen.c lines ~190–350) into a stateless conversion function.

The key mapping logic is:

```c
MachineOperand machine_op_from_ir(TCCIRState *ir, const IROperand *op)
{
    MachineOperand m = {0};
    m.vreg = irop_get_position(*op);
    m.is_64bit = irop_is_64bit(*op);
    // Extract type from op...

    if (irop_get_tag(*op) == IROP_TAG_IMM32) {
        m.kind = MACH_OP_IMM;
        m.u.imm.val = irop_get_imm32(*op);
        return m;
    }

    // Look up register allocation for this vreg
    IRLiveInterval *interval = tcc_ir_live_interval_for_vreg(ir, m.vreg);
    if (!interval) {
        // Constant or special operand
        // ... handle IROP_TAG_STACKOFF, IROP_TAG_SYMREF, etc.
    }

    if (op->pr0_spilled) {
        if (op->is_llocal) {
            // Spilled pointer that needs dereferencing
            m.kind = MACH_OP_SPILL;
            m.needs_deref = true;
            m.u.spill.offset = /* frame offset */;
        } else if (op->is_param) {
            m.kind = MACH_OP_PARAM_STACK;
            m.u.param.offset = /* param offset */;
        } else {
            m.kind = MACH_OP_SPILL;
            m.u.spill.offset = /* spill slot offset */;
        }
    } else if (op->is_local && !op->is_lval) {
        // Address-of local variable (LEA)
        m.kind = MACH_OP_FRAME_ADDR;
        m.u.frame.offset = /* frame offset */;
    } else if (op->is_sym) {
        m.kind = MACH_OP_SYMBOL;
        // ... extract sym + addend
    } else {
        m.kind = MACH_OP_REG;
        m.u.reg.r0 = op->pr0_reg;
        m.u.reg.r1 = m.is_64bit ? op->pr1_reg : -1;
    }

    m.needs_deref = op->is_lval && (m.kind != MACH_OP_SPILL || !op->is_llocal);
    return m;
}
```

**Critical:** This function must produce *exactly* the same materialization decisions as the current `fill_registers_ir` + `materialize_*_ir` combination. Write test assertions that compare old vs. new.

### Step 1.3: Unit tests for `machine_op_from_ir()`

**Action:** Create `tests/ir_tests/test_machine_op.c` (or a pytest test) that verifies:

1. VREG with physical register → `MACH_OP_REG`
2. VREG spilled to stack → `MACH_OP_SPILL`
3. Immediate → `MACH_OP_IMM`
4. Local variable address → `MACH_OP_FRAME_ADDR`
5. Symbol reference → `MACH_OP_SYMBOL`
6. Stack-passed parameter → `MACH_OP_PARAM_STACK`
7. Spilled pointer (is_llocal) → `MACH_OP_SPILL` with `needs_deref=true`
8. 64-bit value in register pair → `MACH_OP_REG` with both r0/r1
9. 64-bit value partially spilled → correct handling

### Step 1.4: Wire into codegen alongside existing path

**Action:** In `ir/codegen.c`, after the existing `tcc_ir_fill_registers_ir()` calls, add parallel `machine_op_from_ir()` calls and assert that the resulting `MachineOperand.kind` is consistent with the old flags.

```c
// Existing:
tcc_ir_fill_registers_ir(ir, &src1_ir);
// New (validation only, remove after Phase 2):
MachineOperand m_src1 = machine_op_from_ir(ir, &src1_ir_orig);
assert(validate_machine_op_vs_filled_ir(&m_src1, &src1_ir));
```

This runs both paths in parallel during the transition, catching any divergence immediately.

### Step 1.5: Integrate into build

**Action:** Add `ir/machine_op.c` to the Makefile (specifically `TINYCC_IR_SRC` or equivalent).

```bash
make cross -j16 && make test -j16
```

## Design Rationale

### Why not just clean up IROperand flags?

The flags encode *allocation state* (which register, whether spilled) mixed with *semantic state* (is_local, is_lval, is_param). These concerns should be separated. `IROperand` should stay as the IR-level representation; `MachineOperand` is the backend-level view after allocation.

### Why a separate struct instead of extending IROperand?

`IROperand` is packed to 10 bytes for cache efficiency during IR passes. `MachineOperand` is only created during codegen (one instruction at a time) and can afford to be larger and clearer.

### Why not just pass allocation metadata separately?

The whole point is to avoid the "test 5 flags in combination" pattern. A single `kind` enum replaces all flag combinations.

## Verification Checklist

- [x] `ir/machine_op.h` created with `MachineOperand` type (`MACH_OP_REG`, `MACH_OP_SPILL`, `MACH_OP_IMM`, `MACH_OP_FRAME_ADDR`, `MACH_OP_SYMBOL`, `MACH_OP_PARAM_STACK`)
- [x] `machine_op_from_ir()` implemented and handles all 6 operand categories
- [x] `ir/machine_op.c` added to build (included via `libtcc.c`)
- [x] `make cross` compiles without warnings
- [x] `make test -j16` passes (no behavior change — MOP path parallel to old path)
- [ ] `fill_registers_ir` removed from MOP path (blocked — see Remaining Work below)

## Remaining Work

### Why `fill_registers_ir` still runs before `machine_op_from_ir`

`fill_registers_ir` does **more** than just copy `allocation.r0` into `pr0_reg`. It also:

1. **Transforms `is_lval`/`is_local`/`is_param` flags** — register-resident params get `is_lval` cleared; pointer-deref operands keep it.
2. **Applies VLA stack-offset deltas** — when `is_local && is_llocal && IROP_TAG_STACKOFF`, the payload offset is adjusted by `old_stackoff - interval->original_offset`.
3. **Handles struct types** — stores `interval->allocation.offset` into `op->u.s.aux_data` instead of `op->u.imm32`.
4. **Stack-passed parameter detection** — sets tag to `IROP_TAG_STACKOFF` + `is_param=1` + `is_local=1` for params where `incoming_reg0 < 0 && allocation.r0 == PREG_NONE`.

`machine_op_from_ir` currently reads the **already-filled** IROperand (i.e., `fill_registers_ir` has already run). The "case 0 direct VREG lookup" added to `machine_op_from_ir` is **inert** in the current flow — by the time `machine_op_from_ir` is called, `fill_registers_ir` has already changed the tag from `IROP_TAG_VREG` to either `IROP_TAG_STACKOFF` (spilled) or set `pr0_reg != PREG_REG_NONE` (in-register), so case 0 is never triggered.

### Path to removing `fill_registers_ir` from MOP path

Option A (correct but complex): Replicate all four transforms above inside `machine_op_from_ir` case 0, then skip `fill_registers_ir` for MOP ops. Requires careful handling of struct types, VLA delta, and param area detection edge cases.

Option B (simpler): Keep `fill_registers_ir` running unconditionally. `machine_op_from_ir` builds `MachineOperand` from the already-filled IROperand. This works correctly and removes the need to replicate the semantic transforms. The benefit of this approach is that `fill_registers_ir` becomes the "conversion kernel" and `machine_op_from_ir` becomes a clean structuring step on top. Full deletion of `fill_registers_ir` is deferred to when ALL instruction paths use MOP (end of Phase 2), at which point the entire fill step disappears.

**Current approach:** Option B. `fill_registers_ir` is called unconditionally; `machine_op_from_ir` converts the result into a `MachineOperand`. The case 0 direct-lookup code is dead and should be removed in the next cleanup commit.
