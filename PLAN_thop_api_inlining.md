# Refactoring Plan for `thop_emit` and Instruction Dispatch

*(Note: This plan is stored for future reference. No code changes are to be executed at this time.)*

## 1. Objective
Refactor the Thumb instruction emitters (`th_***` functions) and the dynamic dispatch mechanism in `arm-thumb-gen.c` to maximize compile-time evaluation. The goal is to allow the compiler to unroll the `thop_emit` loop and completely eliminate irrelevant constraint checks at compile time, trading a slight increase in `.rodata` for improved code generation speed.

## 2. The Compile-Time Constraint
Currently, if the variants array is defined in the `.c` file, the compiler cannot see its contents when compiling `arm-thumb-gen.c` (without LTO). It will see an opaque pointer and will be forced to evaluate the `thop_emit` loop and all its `if` statements at runtime.

**Solution:** To achieve maximum compile-time evaluation, we must move the `thop_variant_shape` definitions and the `TH_TABLE` variant arrays into the **header files** as `static const`. This allows the `static inline` `th_***` functions to expose the exact, constant table data to the compiler, allowing it to unroll the loop and optimize away branches.

## 3. Implementation Steps

### Phase 1a: Expose Tables and Shapes in Headers
- Move all `static const thop_variant_shape` definitions from `.c` files to their respective `.h` files.
- Redefine `TH_TABLE` (or add a new macro) to declare the variant array as `static const` in the header.
- Move the `th_***` wrapper functions into the `.h` files as `static inline` (**not** `always_inline` — let the compiler decide based on its own heuristics; forced inlining of every wrapper causes code bloat at every call site).
- *Result:* The `.c` files will mostly just include the headers, and the compiler will have full visibility of the instruction shapes when compiling the code generator.

### Phase 1b: Fast Paths for Hottest Wrappers
- Add hand-written fast paths to the 5-8 hottest `th_*` wrappers (`th_add_imm`, `th_sub_imm`, `th_mov_imm`, `th_mov_reg`, `th_add_reg`, `th_ldr_imm`, `th_str_imm`).
- These bypass the generic `thop_emit` loop for the common case (low registers, small immediates, T16 encoding).
- Fast paths are 4-6 instructions and **should** use `always_inline` since they are small enough to always be profitable.
- Include debug-mode assertions that validate fast path output against the generic engine.
- *Result:* Each phase delivers measurable, positive value independently. Phase 1a alone may not improve performance (and could regress due to icache pressure from inlining); Phase 1b ensures an immediate win.

### Phase 2: Refactor Function Pointers out of `arm-thumb-gen.c`
- `arm-thumb-gen.c` currently uses structs of function pointers like `ThumbDataProcessingHandler`.
- Indirect function calls via pointers completely block inlining and compile-time evaluation.
- We will replace these structs and function pointers with direct `switch (op)` statements.
- The `emit_alu_imm_for_op` / `emit_alu_reg_for_op` helpers should be `static` (not forced inline) since `op` is a runtime variable — the compiler should decide whether to inline the 11-case switch.
- *Example:*
  ```c
  // Old
  handler.imm_handler(rd, rn, imm, flags, enc);

  // New
  switch (op->opcode) {
      case TOK_ADD: th_add_imm(rd, rn, imm, flags, enc); break;
      case TOK_SUB: th_sub_imm(rd, rn, imm, flags, enc); break;
      // ...
  }
  ```

### Phase 3: Micro-Optimize `thop_emit`
- Mark `thop_emit_error` as `__attribute__((cold, noinline))` to keep error paths out of the icache.
- Pass `thop_args` by `const thop_args *` instead of by value (48 bytes exceeds register-passing capacity).
- Thread `target_feat` through `thop_emit` only if benchmarking shows redundant global loads.
- Benchmark with `perf` / `hyperfine` and iterate.
- All references to `s->size`, `s->rd_place`, etc., will become compile-time constants when inlined.

## 4. Rollback Criteria

Each phase must be independently beneficial. Stop or adjust if:
- `.text` segment grows >15%: switch from `always_inline` to `static inline` on any remaining forced-inline wrappers.
- Compile-time benchmark shows regression after Phase 1a: proceed directly to Phase 1b fast paths before continuing Phase 2.
- Total speedup after all phases is <5%: the bottleneck is elsewhere (parser/preprocessor), stop optimizing codegen dispatch.
