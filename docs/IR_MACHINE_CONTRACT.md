# IR ↔ Machine Code Contract

## Purpose
Define the explicit interface between the IR code generator (`tcc_ir_generate_code()`) and every backend so that spill handling and address materialization no longer rely on ambiguous `SValue` flag combinations.

## Core rules
1. **Value operands** handed to any backend helper must already be either:
   - a literal immediate (`VT_CONST` without `VT_SYM`), or
   - a concrete physical register number recorded in `pr0/pr1` (`PREG_NONE` and `PREG_SPILLED` are illegal at this boundary).
2. **Address operands** must be explicit:
   - preferred form is a register that holds the final address;
   - backend-specific base+offset pairs are allowed only if they are explicit about which reg/offset they use and never reuse spill-slot encodings.
3. **Destinations** must be real registers before the backend runs. If the register allocator placed the dest in a spill slot, IR must allocate a scratch register, rewrite the dest to that register, and remember to store the result back after the backend op completes.
4. **Backend helpers never interpret spill slots.** Loading from / storing to spill slots is the IR layer’s job, performed just before/after invoking backend instructions.

## Materialization responsibilities in IR
All materialization happens inside `tcc_ir_generate_code()` after register allocation (`tcc_ir_fill_registers()`) finishes:

- `materialize_value(src)` converts any spilled or memory-backed operand into an immediate or register, inserting loads when necessary.
- `materialize_addr(src)` produces a register that holds an address, handling stack-slot address-of patterns and two-level indirections.
- `materialize_dest(dest)` guarantees writable registers for results and records deferred storeback operations for spilled destinations (including 64-bit register pairs).

## Minimal machine API needed by IR
To keep the above target-independent, each backend must expose a compact helper surface that IR can call:

- scratch register allocation with optional save/restore hooks,
- load/store from a spill slot (frame-relative),
- compute the address of a stack slot (frame-relative + constant offset),
- optional helpers for constant materialization that already exist today (e.g. literal pool loaders).

Declarations will live in `tcc.h`, with implementations supplied per backend (Thumb can reuse `get_scratch_reg_with_save()`, `tcc_gen_machine_store_to_stack()`, etc.).

## Backend simplifications unlocked
Once IR obeys this contract, Thumb (and other targets) can delete (and Thumb already has):

- the old `tcc_ir_preload_spills()` / `tcc_ir_storeback_spill()` helpers in `arm-thumb-gen.c`,
- spill-aware branches in `load_to_dest()` / `store()`,
- heuristics that guess semantics from `VT_LOCAL` / `VT_LVAL` / `PREG_SPILLED`.

Backends become responsible only for true loads/stores between registers/immediates and concrete addresses, which eliminates the current semantic guessing layer.
As a guardrail, the remaining Thumb helpers now error out immediately if `PREG_SPILLED` ever reaches them, so contract violations show up as compiler bugs instead of latent miscompilations.
