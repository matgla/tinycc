# GCC Nested Functions Support — Implementation Plan

## Problem Statement

```
❯ python run.py -c ../gcctestsuite/gcc-testsuite/gcc/testsuite/gcc.c-torture/execute/20000822-1.c --cflags="-O0"
Using CFLAGS: -O0
Compilation failed:
  20000822-1.c:15: error: cannot use local functions
```

TinyCC rejects GCC nested functions with a hard error at `tccgen.c:11393`. This plan adds full support including captured variables and trampolines for ARMv8-M (Cortex-M33).

## Architecture Decision: Save-Tokens + Reparse

We reuse TCC's inline function model (`skip_or_save_block` + `begin_macro` replay) rather than trying to suspend/resume `gen_function()` mid-compilation. See [Phase 1](phase1_parser.md) for rationale.

## Phases

| Phase | File | Summary | Effort |
|-------|------|---------|--------|
| 1 | [phase1_parser.md](phase1_parser.md) | Save nested func bodies as tokens, reparse after parent `block(0)` | 2-3 days |
| 2 | [phase2_static_chain.md](phase2_static_chain.md) | R10 static chain, captured variable access, pre-scan marking | 3-5 days |
| 3 | [phase3_trampolines.md](phase3_trampolines.md) | Static `.text` trampoline + `.data` chain slot for address-of | 5-7 days |
| 4 | [phase4_ir.md](phase4_ir.md) | IR integration: chain vreg, optimization safety, SET_CHAIN | 3-4 days |
| 5 | [phase5_arm_codegen.md](phase5_arm_codegen.md) | Thumb-2 codegen: prologue, chain load/store, trampoline emit | 3-5 days |
| 6 | [phase6_linker.md](phase6_linker.md) | Linker: R_ARM_ABS32 relocs, STB_LOCAL symbols | 1-2 days |
| 7 | [phase7_testing.md](phase7_testing.md) | Incremental test plan + GCC torture test integration | 3-5 days |

## Recommended Implementation Order

Phases are interleaved in practice:

1. **Phase 1 + Phase 4 (core) + Phase 5 (stub)** → `nested_basic.c` works (no capture)
2. **Phase 2 + Phase 4 (capture) + Phase 5 (chain codegen)** → `nested_capture_*.c` works
3. **Phase 3 + Phase 5 (trampoline) + Phase 6** → `20000822-1.c` works
4. **Phase 7** → Full GCC torture suite validation

## Milestones

| Milestone | Target | Tests Passing |
|-----------|--------|---------------|
| M1 (~1 week) | Direct nested function calls, no capture | `nested_basic.c` |
| M2 (~2 weeks) | Captured variable read/write | `nested_capture_read.c`, `nested_capture_write.c` |
| M3 (~3.5 weeks) | Trampoline support | `20000822-1.c`, `nested_funcptr.c` |
| M4 (~4.5 weeks) | All applicable GCC torture tests | 10-14 of 14 tests |

## Test Cases

Test source files are in [tests/](tests/). Each test targets specific phases:

| Test File | Phases | Description |
|-----------|--------|-------------|
| [nested_basic.c](tests/nested_basic.c) | 1 | No capture, direct call |
| [nested_basic_args.c](tests/nested_basic_args.c) | 1 | Nested function with arguments |
| [nested_multiple.c](tests/nested_multiple.c) | 1 | Multiple nested functions in one parent |
| [nested_capture_read.c](tests/nested_capture_read.c) | 1+2 | Read parent variable |
| [nested_capture_write.c](tests/nested_capture_write.c) | 1+2 | Write parent variable |
| [nested_capture_multiple.c](tests/nested_capture_multiple.c) | 1+2 | Capture multiple variables |
| [nested_capture_array.c](tests/nested_capture_array.c) | 1+2 | Capture array/pointer |
| [nested_direct_call_args.c](tests/nested_direct_call_args.c) | 1+2 | Arguments + captures combined |
| [nested_funcptr.c](tests/nested_funcptr.c) | 1+2+3 | Address-of + trampoline |
| [nested_funcptr_indirect.c](tests/nested_funcptr_indirect.c) | 1+2+3 | Nested func passed through another function |
| [nested_funcptr_call_twice.c](tests/nested_funcptr_call_twice.c) | 1+2+3 | Call via function pointer multiple times |
| [nested_multi_level.c](tests/nested_multi_level.c) | 1+2 | f → g → h chain |
| [nested_recursive_parent.c](tests/nested_recursive_parent.c) | 1+2+3 | Recursive parent with nested func |
| [nested_shadowing.c](tests/nested_shadowing.c) | 1+2 | Local shadows parent variable |
| [nested_struct_return.c](tests/nested_struct_return.c) | 1+2 | Nested function returns struct |

## Affected GCC Torture Tests (14 total)

| Test | Features | Status |
|------|----------|--------|
| `20000822-1.c` | Capture + address-of + indirect call | Target for M3 |
| `920428-2.c` | Capture | Target for M2 |
| `920501-7.c` | Capture | Target for M2 |
| `920612-2.c` | Capture | Target for M2 |
| `921017-1.c` | Capture | Target for M2 |
| `921215-1.c` | Capture | Target for M2 |
| `931002-1.c` | Capture | Target for M2 |
| `nestfunc-1.c` | Basics | Target for M1 |
| `nestfunc-2.c` | Arguments | Target for M1 |
| `nestfunc-3.c` | Struct returns | Target for M2 |
| `comp-goto-2.c` | Computed goto | Deferred (needs computed goto) |
| `nestfunc-5.c` | `__label__` | Deferred (needs nonlocal goto) |
| `nestfunc-6.c` | Nonlocal goto | Deferred (needs nonlocal goto) |
| `pr24135.c` | `__label__` + nonlocal goto | Deferred (needs nonlocal goto) |

## Key Codebase Context

### Current error location
```c
// tccgen.c:11391-11393
if (tok == '{') {
    if (l != VT_CONST)
        tcc_error("cannot use local functions");
```

### Global state to save/restore

| Global | Type | Purpose |
|--------|------|---------|
| `tcc_state->ir` | `TCCIRState*` | Current IR state |
| `loc` | `int` | Local stack offset |
| `ind` | `int` | Code output index |
| `rsym` | `int` | Return symbol chain |
| `func_ind` | `int` | Function start index |
| `funcname` | `const char*` | Function name |
| `func_vt` | `CType` | Return type |
| `func_var` | `int` | Variadic flag |
| `cur_scope`, `root_scope`, `loop_scope` | `struct scope*` | Scope chain |
| `local_stack` | `Sym*` | Local symbol stack |
| `local_label_stack` | `Sym*` | Local labels |
| `global_label_stack` | `Sym*` | Global labels |
| `nocode_wanted` | `int` | Code suppression |
| `local_scope` | `int` | Scope depth |
| `nb_temp_local_vars` | `int` | Temp local count |
| `arr_temp_local_vars` | `struct[8]` | Temp local info |
| `cur_text_section` | `Section*` | Output section |
| `cur_switch` | `struct switch_t*` | Switch state |

## Risks & Open Questions

1. **Re-entrancy** — Static `.data` chain slots are not re-entrant for recursive parents. Acceptable for now.
2. **Token stream end** — `gen_function()` calls `next()` at end; verify `begin_macro`/`end_macro` handles this.
3. **Symbol mangling** — Use `f1__nested__f2` or internal token IDs to avoid collisions.
4. **Multi-level nesting** — Requires chain-of-chains (each level one pointer indirection).
5. **Inline functions** — Token-save works naturally; trampoline names need uniqueness per instantiation.
6. **Nonlocal goto** — 4 tests deferred; needs stack unwinding support.
7. **Optimization safety** — Chain loads/stores use non-FP base; existing conservative rules should suffice.
8. **Thread safety** — `.data` chain slots not thread-safe; OK for Cortex-M33.
9. **Pre-scan accuracy** — `prescan_captured_vars` over-marks (safe but suboptimal); can refine later.
