## Compact IR Operand Storage (u64-tagged) — Implementation Plan

### Goal
Reduce IR memory usage and remove the bug-prone overloading of `SValue` fields (`r`, `VT_LVAL`, `VT_LOCAL`, `c`, `sym`, spills) by storing IR operands as a compact, explicit tagged value.

Primary constraint: runs natively on 32-bit MCU (16MB RAM). Internal-only change is acceptable.

### Current Problem (why refactor)
- IR instructions currently embed `SValue` operands (see `TACQuadruple` in `tcc.h`). `SValue` includes `CType`, unions, constants, and symbol pointers.
- Addressing semantics are encoded implicitly via combinations like `VT_LOCAL` / `VT_LVAL` and get rewritten during spills/regalloc, causing ambiguous “needs dereference?” decision paths.
- Backends (e.g. Thumb spill preload/storeback) contain special cases because `VT_LOCAL` without `VT_LVAL` can mean address-of, which is wrong for spilled temporaries.

### Design: `IROperand` (uint64_t)
Use a single `uint64_t` payload with a **2-bit tag** so it is portable on 32-bit MCUs and does not rely on pointer tagging or 8-byte pointer alignment.

- `typedef uint64_t IROperand;`
- Low 2 bits are the tag, remaining bits are the payload: `payload = op >> 2`.

Tags (4 kinds):
- `IMM32`: inline signed/unsigned 32-bit immediate
- `VREG`: encoded vreg id (32-bit)
- `STACKOFF`: FP-relative signed 32-bit offset (for spills or explicit stack slots)
- `POOL`: index into a per-function IR operand pool for anything non-inline (i64, f32/f64 bits, symbol+addend, etc.)

**Why 2-bit tags**
- Avoids relying on pointer alignment for tagging on 32-bit ARM M-profile.
- Keeps decoding simple and fast.

### Operand Pool
A per-function pool stores larger or structured values.

Pool entry kinds (minimal set):
- `I64`: 64-bit integer bits
- `F32`: IEEE float bits
- `F64`: IEEE double bits
- `SYMREF`: `{ Sym* sym, int32_t addend, uint32_t flags }` (flags can carry “addr vs lvalue” nuances if needed)

Pool is append-only during IR build; entries are referred by `IROperand` tag `POOL`.

### Where type info lives
Do **not** store full `CType` per operand in the IR.

- For vregs: keep allocator-relevant type classification in existing `IRLiveInterval` (`is_float`, `is_double`, `is_llong`, `is_lvalue`, `addrtaken`).
- For pool constants: pool entry kind indicates the constant type.
- For stack offsets and symrefs: treat as pointer/int type at expansion time (or infer from op/dest vreg).

### Migration strategy (low-risk)
To avoid rewriting backends immediately:
1. Keep `TACQuadruple` (with `SValue` operands) as a **codegen-facing expanded form**.
2. Introduce a new compact IR instruction storage type (e.g. `IRQuad`) using `IROperand`.
3. During code generation, expand each `IRQuad` into a temporary `TACQuadruple` with minimal `SValue` fields filled:
   - `vr` from `VREG`
   - regalloc results from `IRLiveInterval.allocation`
   - spilled offsets from `IRLiveInterval.allocation.offset`
   - constants/symbols from pool

This yields the biggest memory win (IR instruction storage) without touching target backends early.

### Implementation steps
1. Add `IROperand` encoding helpers and pool entry definitions.
2. Add `IRQuad` storage and swap `TCCIRState.instructions` to store `IRQuad`.
3. Update IR builders/optimizations to read/write `IROperand`.
4. Update regalloc to use vreg allocation data; stop writing `pr0/pr1` into embedded operands.
5. Update codegen loop to expand `IRQuad -> TACQuadruple` for existing backend machine ops.
6. Gradually retire `VT_LVAL`/`VT_LOCAL` heuristics (`tcc_ir_operand_needs_dereference`, spill preload hacks) by making IR semantics explicit (`LOAD`/`STORE`, address materialization).

### Safety notes (32-bit MCU)
- Never use packed structs containing `uint64_t`; misaligned 64-bit accesses can fault.
- Keep `IROperand` stored in naturally aligned structs/arrays.

### Success criteria
- IR instruction memory drops substantially (no embedded `SValue` per instruction).
- No more `VT_LOCAL`/`VT_LVAL` semantic flipping for spills.
- `ir_tests/` still pass after migration.
