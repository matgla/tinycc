# Phase 4: IR Integration & Optimization Safety

**Effort**: 3-4 days
**Files**: `ir/core.c`, `ir/core.h`, `ir/codegen.c`, `ir/live.c`, `tccir.h`, `tccls.c`

## Overview

Add nested function metadata to `TCCIRState`, model the static chain register (R10) as a parameter-like vreg, ensure IR optimizations don't eliminate captured variable accesses, and add the `SET_CHAIN` IR instruction for parent→nested calls.

## TODO

- [x] Add `NestedFunc *nested_funcs`, `nb_nested_funcs`, `nested_funcs_capacity` to `TCCIRState`
- [x] Add `has_static_chain` (uint8_t), `static_chain_vreg` (int), `parent_loc` (int) to `TCCIRState`
- [x] Initialize new fields in `tcc_ir_alloc()`
- [x] Free `nested_funcs` array in `tcc_ir_free()`
- [x] Allocate chain vreg via `tcc_ir_alloc_var()` when `has_static_chain` (using VAR not PARAM to avoid shifting parameter indices)
- [x] Mark chain vreg live-in at instruction 0 with full-function live range
- [x] Set chain vreg `incoming_reg = REG_STATIC_CHAIN` (R10) — like param incoming regs
- [x] Add chain vreg to liveness analysis: mark live-in, extend to all chain load/store uses, precolor to R10
- [x] Add `TCCIR_OP_SET_CHAIN` to `TccIrOp` enum in `tccir.h`
- [x] Define `SET_CHAIN` semantics: "write FP to R10 before next call"
- [x] Add SET_CHAIN to IR dump output
- [x] Fix store path for captured variables in `th_store_resolve_base_ir()`
- [ ] Verify store-load forwarding does NOT apply to chain-relative loads (non-FP base)
- [ ] Verify dead store elimination does NOT remove chain-relative stores (external side effect)
- [ ] Verify constant propagation stops at chain-relative loads
- [ ] Verify CSE CAN optimize chain loads from same offset within a basic block
- [x] Test IR dump output with `--dump-ir` for nested function compilation

## New IR Instruction: `SET_CHAIN`

```
TCCIR_OP_SET_CHAIN    // no operands — implicit: R10 <- FP
```

This is emitted in the **parent** before calling a nested function directly. The codegen lowers it to `MOV R10, R7`.

Alternative: make it explicit with operands: `SET_CHAIN dest=R10, src=FP`. But the implicit form is simpler since the source (FP) and destination (R10) are always the same on ARM.

## Chain Vreg as Parameter-like Entity

The static chain vreg models the R10 register (static chain pointer) as a live-in value at function entry. It is allocated as a **VAR** type vreg (not PARAM) to avoid shifting the actual function parameter indices.

```
// During nested gen_function setup:
function gen_function_nested_setup(ir):
    if not ir->has_static_chain: return

    // Allocate as VAR (not PARAM) to avoid shifting parameter indices
    chain_vreg = tcc_ir_vreg_alloc_var(ir)
    ir->static_chain_vreg = chain_vreg

    // Create a live interval for chain_vreg:
    // - start = 0 (live at entry)
    // - end = last instruction (conservative; could compute tighter range)
    // - incoming_reg = 10 (R10)
    // - addrtaken = 0
    interval = find_or_create_interval(chain_vreg)
    interval->start = 0
    interval->end = ir->next_instruction_index
    interval->incoming_reg0 = 10  // R10
```

## Optimization Safety

Chain-relative loads/stores use a non-FP base register (chain vreg → R10). The existing optimizer conservative rules should apply:

| Optimization | Safe? | Reason |
|-------------|-------|--------|
| Store-load forwarding | YES | Only applies to same-base, same-offset; chain base ≠ FP base |
| Dead store elimination | YES | Only applies to stack locals (FP-relative); chain stores use different base |
| Constant propagation | YES | Cannot propagate through memory loads; chain loads are memory ops |
| CSE (intra-block) | YES | Chain loads from same offset can be CSE'd within a basic block |
| CSE (inter-block) | CAUTION | Safe IF no calls between load and reuse (parent frame unchanged) |
| Copy propagation | YES | Standard rules apply |
| DCE | YES | If chain load result unused, can be eliminated |

**Key insight**: Since captured variable access goes through a vreg (chain_vreg) as base rather than FP, the optimizer already treats these as generic memory operations, not stack locals. No special marking needed for most passes.

**Exception**: Store-load forwarding and dead store elimination are currently conservative — they only optimize stack locals whose address is NOT taken (FP-relative, addrtaken=0). Chain-relative ops use a different base, so they're automatically excluded.

## Pseudocode: Chain-relative IR Generation

```
// No new opcodes — use existing LOAD/STORE with chain_vreg as base:

function emit_chain_load(ir, dest_vreg, parent_offset):
    src = make_operand_vreg_plus_offset(ir->static_chain_vreg, parent_offset)
    dest = make_operand_vreg(dest_vreg)
    tcc_ir_put_op(ir, TCCIR_OP_LOAD, src, NONE, dest)

function emit_chain_store(ir, parent_offset, src_vreg):
    dest = make_operand_vreg_plus_offset(ir->static_chain_vreg, parent_offset)
    src = make_operand_vreg(src_vreg)
    tcc_ir_put_op(ir, TCCIR_OP_STORE, src, NONE, dest)
```

## Pseudocode: Parent Call Chain Setup (IR)

```
// In parent's gfunc_call path:
function gen_call_to_nested(ir, nested_sym, args):
    // Option A: dedicated SET_CHAIN instruction
    emit TCCIR_OP_SET_CHAIN
    emit TCCIR_OP_FUNCCALLVAL nested_sym, args

    // Option B: explicit MOV via vreg
    tmp = alloc_temp_vreg()
    emit TCCIR_OP_ASSIGN tmp <- FP_OPERAND
    // annotate call: R10 must hold `tmp`
    emit TCCIR_OP_FUNCCALLVAL nested_sym, args, extra_reg={R10, tmp}

    // DECISION: Option A (simpler)
```

## Test Cases

- Dump IR with `--dump-ir` for each Phase 2 test and verify chain load/store instructions appear
- Verify chain stores are NOT eliminated by dead store elimination
- Verify chain loads from same offset in same block ARE CSE'd
- Verify SET_CHAIN appears before direct calls to nested functions in parent IR
