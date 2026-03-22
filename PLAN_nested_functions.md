# Plan: Supporting GCC Nested Functions (20000822-1.c)

## Problem Statement

```
❯ python run.py -c ../gcctestsuite/gcc-testsuite/gcc/testsuite/gcc.c-torture/execute/20000822-1.c --cflags="-O0"
Using CFLAGS: -O0
Compilation failed:
  20000822-1.c:15: error: cannot use local functions
```

The test `20000822-1.c` uses **GCC nested functions** — a GNU C extension that allows defining functions inside other functions, with access to the enclosing scope's variables. TinyCC currently rejects this with a hard error at `tccgen.c:11393`.

---

## Test Analysis

```c
/* { dg-require-effective-target trampolines } */
void abort(void);

int f0(int (*fn)(int *), int *p) {
    return (*fn)(p);            // indirect call via function pointer
}

int f1(void) {
    int i = 0;

    int f2(int *p) {            // (1) nested function definition
        i = 1;                  // (2) writes to parent's local variable
        return *p + 1;          // (3) reads *p (which points to i)
    }

    return f0(f2, &i);         // (4) takes address of nested function → trampoline
}

int main() {
    if (f1() != 2)             // expected: f2 sets i=1, returns *(&i)+1 = 2
        abort();
    return 0;
}
```

### GNU C Features Required

| # | Feature | Complexity | Description |
|---|---------|------------|-------------|
| 1 | Nested function definition | Medium | `f2` defined inside `f1`'s body |
| 2 | Parent scope variable capture | High | `f2` reads/writes `i` from `f1`'s stack frame |
| 3 | Address-of nested function | High | `f2` passed as `int (*)(int*)` to `f0` |
| 4 | Trampoline / indirect call | High | `f0` calls `f2` through a function pointer — requires trampoline to set up static chain |

---

## Affected GCC Torture Tests (14 total)

All require `dg-require-effective-target trampolines`:

| Test | Features Used |
|------|---------------|
| `20000822-1.c` | Nested func, capture, address-of, indirect call |
| `920428-2.c` | Nested function with capture |
| `920501-7.c` | Nested function with capture |
| `920612-2.c` | Nested function with capture |
| `921017-1.c` | Nested function with capture |
| `921215-1.c` | Nested function with capture |
| `931002-1.c` | Nested function with capture |
| `comp-goto-2.c` | Nested function + computed goto |
| `nestfunc-1.c` | Nested function basics |
| `nestfunc-2.c` | Nested function arguments |
| `nestfunc-3.c` | Nested function with struct returns |
| `nestfunc-5.c` | Nested function + `__label__` |
| `nestfunc-6.c` | Nested function + nonlocal goto |
| `pr24135.c` | Nested function + `__label__` + nonlocal goto |

---

## Current Codebase State

### Where the error originates

```c
// tccgen.c:11391-11393
if (tok == '{') {
    if (l != VT_CONST)
        tcc_error("cannot use local functions");
```

`decl()` is called with `l = VT_LOCAL` when parsing block-scope declarations.
Only `l = VT_CONST` (file scope) is permitted to have function bodies.

### Compilation pipeline (current)

```
decl(VT_CONST)  →  parse type + declarator  →  gen_function(sym)
                                                    ↓
                                              tcc_ir_alloc()     ← one IR state per function
                                              block(0)           ← parse body, emit IR
                                              optimization passes
                                              register allocation
                                              tcc_ir_codegen_generate()  ← emit Thumb-2
                                              tcc_ir_free()
```

### Global state consumed by gen_function

These globals must be saved/restored when suspending parent compilation:

| Global | Type | Purpose |
|--------|------|---------|
| `tcc_state->ir` | `TCCIRState*` | Current IR state (per-function, alloc'd by `tcc_ir_alloc`) |
| `loc` | `int` | Current local stack offset (grows negative) |
| `ind` | `int` | Current code output index in `cur_text_section` |
| `rsym` | `int` | Return symbol jump chain (-1 sentinel) |
| `func_ind` | `int` | Function start index |
| `funcname` | `const char*` | Current function name |
| `func_vt` | `CType` | Function return type |
| `func_var` | `int` | Variadic flag |
| `cur_scope` | `struct scope*` | Current scope (linked list) |
| `root_scope` | `struct scope*` | Root scope of current function |
| `loop_scope` | `struct scope*` | Current loop scope |
| `local_stack` | `Sym*` | Local symbol stack |
| `local_label_stack` | `Sym*` | Local labels |
| `global_label_stack` | `Sym*` | Global label stack (saved per-function) |
| `nocode_wanted` | `int` | Code generation suppression flag |
| `local_scope` | `int` | Local scope depth counter |
| `nb_temp_local_vars` | `int` | Temp local variable count |
| `arr_temp_local_vars` | `struct[8]` | Temp local variable info |
| `cur_text_section` | `Section*` | Current output section |
| `cur_switch` | `struct switch_t*` | Current switch (should be NULL at nested func) |

### Key constraints

- **One `TCCIRState` per function** — nested function compilation would need to suspend the parent's state
- **No static chain concept** — IR locals are simple FP offsets with no cross-frame access
- **No trampoline infrastructure** — no code exists for generating executable trampolines
- **ARM FP register is R7** (Thumb convention), not R11 — affects static chain register choice
- **Inline functions** already use `skip_or_save_block` + reparse model — we should reuse this pattern

### ARM calling convention (AAPCS)

- R0-R3: argument registers
- R7: frame pointer (Thumb)
- R12 (IP): scratch / intra-procedure call
- R10: platform register (available as static chain in GCC)
- LR (R14): link register
- No existing use of R10 as static chain

---

## Architecture Decision: Save-Tokens + Reparse (like inline functions)

### Why not suspend/resume?

Suspending the parent's `gen_function()` mid-compilation (saving all globals, allocating a new `TCCIRState`, compiling the nested function, restoring) is fragile:

- `gen_function()` has deep call stacks: `gen_function → block → block → decl → ???`
- The C stack state (return addresses, local variables in `block()`, `decl()`, etc.) cannot be saved
- Many optimization passes assume they run on a complete function — partial IR state is invalid

### Why save-tokens + reparse?

TCC already has a proven model: **inline functions**. When a `static inline` function is encountered, TCC:

1. Calls `skip_or_save_block(&fn->func_str)` to tokenize the entire body
2. Stores the `TokenString` for later
3. When the function is actually used, replays via `begin_macro(fn->func_str, 1)` + `gen_function()`

We use the **same pattern** for nested functions:

1. When we see a nested function definition inside `decl(VT_LOCAL)`, save its body as a `TokenString`
2. Record metadata (captured variables, parent scope info)
3. Jump past the body (the parent continues parsing normally)
4. **Before** the parent's `gen_function()` returns (after `block(0)` but before optimizations), compile all nested functions

### What about VLA-style token caching?

VLAs also use `skip_or_save_block` for array dimension expressions (`vla_array_tok`). The nested function approach is the same concept at a larger scale — we're caching a complete function body instead of a single expression.

### Storage: NestedFunc array on TCCIRState

We store nested function descriptors in an array on the parent's `TCCIRState`, similar to how `inline_fns` are stored on `TCCState`:

```c
typedef struct NestedFunc {
    TokenString *func_str;      // saved token stream of body
    Sym *sym;                   // symbol (with mangled name like f1.f2)
    CType func_type;            // function type
    int *captured_offsets;      // parent FP offsets of captured vars
    int nb_captured;            // number of captured vars
    int trampoline_needed;      // 1 if address-of is taken
    char parent_filename[1];    // filename for error reporting
} NestedFunc;
```

---

## Implementation Plan

### Phase 1: Parser — Save Nested Function Bodies as Tokens

**Effort**: 2-3 days
**Files**: `tccgen.c`, `tcc.h`, `tccir.h`

#### 1.1 Data structures

```c
// tcc.h additions:

// Nested function descriptor — stored before compilation
typedef struct NestedFunc {
    TokenString *func_str;        // saved token stream of function body
    Sym *sym;                     // function symbol in parent's local scope
    CType type;                   // full function type
    AttributeDef ad;              // function attributes
    int v;                        // token id (function name)
    char filename[256];           // source filename for error messages
} NestedFunc;

// tccir.h additions to TCCIRState:
//   NestedFunc *nested_funcs;
//   int nb_nested_funcs;
//   int has_static_chain;      // 1 if this function is itself nested
//   int static_chain_vreg;     // vreg holding the chain (R10 on entry)
```

#### 1.2 Pseudocode: Modify `decl(VT_LOCAL)` to save nested function body

```
function decl(l):
    ...existing type parsing...

    if tok == '{':
        if l == VT_LOCAL:
            // ── NEW: nested function definition ──
            assert (type.t & VT_BTYPE) == VT_FUNC

            // Validate parameters (same as file-scope path)
            foreach param in type.ref->next:
                if param has no identifier: error("expected identifier")
                if param is void: param.type = int_type

            merge_funcattr(&type.ref->f, &ad.f)

            // Create a mangled symbol: "parent.child"
            mangled_name = concat(funcname, ".", get_tok_str(v))

            // Push symbol into LOCAL scope so the parent body can reference it
            type.t &= ~VT_EXTERN
            sym = sym_push(v, &type, VT_CONST, 0)  // VT_CONST: it's a function
            put_extern_sym(sym, cur_text_section, 0, 0)  // placeholder

            // Save the token stream (reuse inline function pattern)
            ir = tcc_state->ir
            nf = &ir->nested_funcs[ir->nb_nested_funcs++]
            nf->sym = sym
            nf->type = type
            nf->ad = ad
            nf->v = v
            strcpy(nf->filename, file->filename)
            skip_or_save_block(&nf->func_str)  // saves '{' ... '}'

            break  // continue parsing parent body
        else:
            // existing file-scope path
            ...
```

#### 1.3 Pseudocode: Compile nested functions after parent body

Insert nested function compilation in `gen_function()`, **after** `block(0)` returns but **before** IR optimization. At this point:
- The parent's `loc` is finalized (all locals allocated)
- Captured variable FP-offsets are known
- The parent's token stream is exhausted (nested body was already skipped)

```
function gen_function(sym):
    ...existing setup...

    ir = tcc_ir_alloc()
    tcc_state->ir = ir
    ...existing param processing...
    block(0)
    tcc_ir_backpatch_to_here(ir, rsym)

    // ── NEW: compile nested functions ──
    if ir->nb_nested_funcs > 0:
        compile_nested_functions(ir, sym)

    ...existing optimization passes...
    ...existing register allocation...
    ...existing codegen...
    tcc_ir_free(ir)

function compile_nested_functions(parent_ir, parent_sym):
    // Save ALL parent global state
    saved = {
        .ir          = tcc_state->ir,
        .loc         = loc,
        .ind         = ind,
        .rsym        = rsym,
        .func_ind    = func_ind,
        .funcname    = funcname,
        .func_vt     = func_vt,
        .func_var    = func_var,
        .cur_scope   = cur_scope,
        .root_scope  = root_scope,
        .loop_scope  = loop_scope,
        .local_stack = local_stack,
        .local_label_stack = local_label_stack,
        .global_label_stack = global_label_stack,
        .nocode_wanted = nocode_wanted,
        .local_scope = local_scope,
        .nb_temp_local_vars = nb_temp_local_vars,
        .cur_text_section = cur_text_section,
        .cur_switch = cur_switch,
    }
    memcpy(saved.arr_temp_local_vars, arr_temp_local_vars, sizeof arr_temp_local_vars)

    // Record parent's finalized stack layout for capture resolution
    parent_loc = loc   // deepest local offset — all offsets are known

    for each nf in parent_ir->nested_funcs:
        // Replay the saved token stream (same as inline function expansion)
        tccpp_putfile(nf->filename)
        begin_macro(nf->func_str, 1)
        next()  // prime the first token

        // The nested function compiles into the SAME text section
        cur_text_section = saved.cur_text_section

        // gen_function() handles everything: IR alloc, block(), optimize, codegen
        gen_function(nf->sym)

        end_macro()

    // Restore ALL parent state
    tcc_state->ir    = saved.ir
    loc              = saved.loc
    ind              = saved.ind
    rsym             = saved.rsym
    func_ind         = saved.func_ind
    funcname         = saved.funcname
    func_vt          = saved.func_vt
    func_var         = saved.func_var
    cur_scope        = saved.cur_scope
    root_scope       = saved.root_scope
    loop_scope       = saved.loop_scope
    local_stack      = saved.local_stack
    local_label_stack = saved.local_label_stack
    global_label_stack = saved.global_label_stack
    nocode_wanted    = saved.nocode_wanted
    local_scope      = saved.local_scope
    nb_temp_local_vars = saved.nb_temp_local_vars
    cur_text_section = saved.cur_text_section
    cur_switch       = saved.cur_switch
    memcpy(arr_temp_local_vars, saved.arr_temp_local_vars, sizeof arr_temp_local_vars)
```

#### 1.4 Why after `block(0)` but before optimizations?

- **After `block(0)`**: All parent locals have been allocated, so we know exact FP offsets for captured variables. The token stream has been fully consumed.
- **Before optimizations**: The parent's IR is complete but not yet optimized. Nested function code goes into the `.text` section at `ind` (which gen_function modifies). After we restore `ind`, the parent's codegen continues where it left off.
- **Note**: `gen_function()` calls `next()` at the end which consumes the closing `}`. Since we use `begin_macro/end_macro` to replay, this is handled correctly — the nested function body is self-contained in the `TokenString`.

#### 1.5 Symbol visibility during parent body parsing

After `skip_or_save_block`, the nested function's symbol (`f2`) is on `local_stack`. When the parent body references `f2` (e.g., `f0(f2, &i)`), it resolves via `sym_find()` to a function symbol — just like any other function. No special handling needed for **direct calls**.

For **address-of** (`&f2` or passing `f2` as function pointer), the symbol resolution produces a function reference. The trampoline logic (Phase 3) intercepts this.

---

### Phase 2: Static Chain — Captured Variable Access

**Effort**: 3-5 days
**Files**: `tccgen.c`, `tcc.h`, `tccir.h`, `ir/core.c`, `ir/core.h`, `tccls.c`, `arch/armv8m.c`

#### 2.1 Static chain register: R10

Following GCC's ARM convention, use **R10** as the static chain register. When a nested function is called, R10 points to the parent's stack frame (= parent's FP value at the time of the call).

```c
// arm-thumb-defs.h
#define REG_STATIC_CHAIN  10  // R10: static chain for nested functions
```

#### 2.2 Architecture config addition

```c
// arch/armv8m.c — extend ArchitectureConfig
ArchitectureConfig architecture_config = {
    .pointer_size = 4,
    .stack_align = 8,
    .reg_size = 4,
    .parameter_registers = 4,
    .has_fpu = 0,
    .static_chain_reg = 10,   // NEW: R10 for nested function static chain
};
```

#### 2.3 Identifying captured variables

During the reparse of the nested function body (inside `gen_function` called for the nested func), variable lookups that resolve to parent-scope locals need special treatment.

**Problem**: After `skip_or_save_block` saved the nested function's tokens and we later replay them, `sym_find()` for captured variables must still resolve. But `pop_local_syms(NULL, 0)` in the parent's `gen_function()` hasn't run yet (we compile nested functions before that). So the parent's local symbols are still on `local_stack`.

**Approach**: We need a way to detect "this symbol is from the parent scope, not our own scope" during nested function compilation.

```
// Pseudocode for captured variable detection:

// Before compiling nested function, save the boundary of the parent's local_stack
parent_locals_boundary = local_stack  // top of parent's locals

// During nested function compilation, in sym_find/variable resolution:
function resolve_var_in_nested_func(tok):
    sym = sym_find(tok)
    if sym == NULL: return NULL

    if sym belongs to parent scope (sym->prev chain crosses parent_locals_boundary):
        // This is a captured variable
        mark_as_captured(sym)
        return create_chain_access(sym)  // returns an SValue with chain-relative addressing
    else:
        return sym  // local to nested function, normal access
```

**Alternative simpler approach**: Since we know the nested function's own locals are pushed after we enter `gen_function(nf->sym)`, any `VT_LOCAL` symbol that was already on the stack at entry is a parent local:

```
// Pseudocode:
// In compile_nested_functions(), before calling gen_function(nf->sym):
parent_local_stack_top = local_stack   // save parent's local stack position

// Inside the nested gen_function, if we resolve a VT_LOCAL sym:
if sym->r & VT_LOCAL && sym is on local_stack && sym was pushed before parent_local_stack_top:
    // This is a captured variable access
    // sym->c is its FP-relative offset in the parent's frame
    // Emit: LOAD/STORE via R10 (static chain) + sym->c
```

#### 2.4 Captured variable IR generation

When we detect a captured variable access inside a nested function, instead of the normal `VT_LOCAL | VT_LVAL` SValue (which means "FP + offset"), we produce an SValue that means "chain_reg + offset":

```
// Pseudocode for generating IR for captured variable access:

function svalue_for_captured_var(sym):
    // Option A: New SValue kind — VT_CHAIN_LOCAL
    sv.r = VT_CHAIN_LOCAL | VT_LVAL    // new flag meaning "relative to static chain reg"
    sv.c.i = sym->c                     // parent FP offset (already known)
    sv.type = sym->type
    return sv

    // Option B: Reuse VT_LOCAL but with a different base register hint
    // The IR emitter checks ir->has_static_chain when it sees a VT_LOCAL
    // and the sym_scope indicates parent scope → redirect to chain reg
```

**Option B is simpler** — it avoids a new SValue kind. We distinguish captured variables by checking if the symbol's scope is outside the current function.

#### 2.5 IR-level handling of captured variables

No new IR opcodes needed. Captured variable access becomes:

```
// Normal local:   LOAD dest, [FP + offset]    → FP is implicit base for VT_LOCAL
// Captured local: LOAD dest, [V_chain + offset] → V_chain is a vreg holding R10

// In IR generation (tccir.c or tccgen.c), when loading a captured var:
// 1. The static chain vreg is allocated once at function entry
// 2. Captured access: emit TCCIR_OP_LOAD with src1 = chain_vreg, offset = parent_offset
```

Pseudocode for chain vreg setup:

```
function gen_function_for_nested(sym):
    ...standard gen_function() setup...

    if sym is a nested function (ir->has_static_chain):
        // Allocate a vreg that holds R10 (static chain)
        // This vreg is live for the entire function
        ir->static_chain_vreg = tcc_ir_alloc_vreg(ir, IR_TYPE_PTR)

        // Emit IR instruction that says "chain_vreg = R10 on entry"
        // This is like a parameter but in R10 instead of R0-R3
        emit TCCIR_OP_ASSIGN chain_vreg <- STATIC_CHAIN_REG
```

#### 2.6 Register allocation changes

```
// Pseudocode for register allocator changes:

function tcc_ls_allocate_registers(ls, params, float_params, spill_base):
    ...existing setup...

    if current function has_static_chain:
        // Remove R10 from the allocatable register set
        ls->registers_map &= ~(1ULL << 10)

        // The chain vreg must be assigned to R10
        // Mark it with incoming_reg = R10 (similar to how params get R0-R3)
        chain_interval = find_interval_for_vreg(ls, ir->static_chain_vreg)
        chain_interval->r0 = 10  // pre-assigned to R10
```

#### 2.7 Captured variable marking in parent

Variables captured by nested functions must be forced to stack (cannot be register-only):

```
// Pseudocode: In compile_nested_functions(), after parsing all nested func bodies
// but we actually need this DURING block(0) of the parent...

// Better approach: During the first parse of the parent body, whenever we
// define a nested function via skip_or_save_block(), we can't yet know which
// parent vars are captured (we haven't parsed the nested body yet!)

// Solution: Two-pass or lazy capture marking:
//
// OPTION A — Lazy: During nested function gen_function(), when we encounter
// a captured var access, set sym->addrtaken = 1 on the parent's symbol.
// Since the parent's IR is already generated, we need to retroactively fix
// the parent's liveness info to mark these as spilled.
//
// OPTION B — Pre-scan: After skip_or_save_block() saves the nested body tokens,
// do a quick token scan looking for identifier references that match parent locals.
// Mark those as captured immediately.
//
// OPTION C — Reparse approach (simplest, matches our architecture):
// Since nested functions are compiled AFTER the parent's block(0) but BEFORE
// optimization, the parent's IR is complete. At this point:
// - Parent locals have known FP offsets (loc is finalized)
// - We compile the nested function which uses these offsets via chain reg
// - The parent never needs to "know" about captures — the nested function
//   accesses parent memory through R10, which is transparent to the parent
//
// Wait — there IS a problem: if the parent's register allocator puts a
// "captured" variable in a register only and never spills it, the nested
// function's R10-relative access would read stale stack memory.
//
// SOLUTION: Mark variables as addrtaken in the parent's IR generation.
// During block(0), when we encounter a nested function that MIGHT capture
// parent vars, conservatively mark ALL parent locals as addrtaken.
// Or better: do a token pre-scan of the saved body to find which vars are used.

function prescan_captured_vars(nf, parent_local_stack):
    // Walk the saved TokenString looking for identifiers
    // that match parent local variable names.
    // Mark matching parent syms as addrtaken (forces stack spill).

    tokens = tok_str_buf(nf->func_str)
    pos = 0
    while tokens[pos] != TOK_EOF:
        t = tokens[pos]
        if t >= TOK_IDENT:
            sym = lookup in parent_local_stack for token t
            if sym != NULL && sym->r & VT_LOCAL:
                sym->type.t |= VT_ADDRTAKEN   // force to stack
                // Record in nf->captured_offsets for later
                nf->captured_offsets[nf->nb_captured++] = sym->c  // FP offset
        pos = advance past token + associated data

    // This runs during decl(VT_LOCAL) right after skip_or_save_block,
    // BEFORE the parent's block(0) continues parsing. So the addrtaken
    // flag is set BEFORE the parent's IR generation decisions.
```

**Critical insight**: The pre-scan must happen at parse time (during `decl(VT_LOCAL)`) before the parent's `block(0)` generates IR for variables that might be captured. Otherwise the parent's IR could put them in registers.

#### 2.8 Direct call convention for nested functions

When the parent calls a nested function directly (not via function pointer):

```
// Parent's IR for: f2(arg)
// 1. Load R10 = current FP (R7)
//    MOV R10, R7   — or emit IR: ASSIGN R10 <- FP
// 2. Normal call: BL f1.f2

// Pseudocode in tccgen.c gfunc_call path:
function gen_call(func_sym, args):
    if func_sym is a nested function:
        // Set up static chain before call
        emit IR: STORE R10, current_FP  (or MOV R10, R7)
        // Then proceed with normal call
    emit IR: FUNCCALLVAL func_sym, args...
```

The IR can represent this as a regular `FUNCCALLVAL` where the call site metadata records "needs chain setup". Or emit a new `TCCIR_OP_SET_CHAIN` instruction before the call.

---

### Phase 3: Trampoline Generation (Address-of Nested Function)

**Effort**: 5-7 days
**Files**: `tccgen.c`, `arm-thumb-gen.c`, `arm-thumb-opcodes.c`, `tccelf.c`

This is the most complex phase. Required when a nested function's address is taken (e.g., `f0(f2, &i)` where `f2` is passed as a function pointer).

#### 3.1 Why not executable stack trampolines?

GCC's approach generates small code snippets on the stack. Ruled out for ARMv8-M: the stack is non-executable when MPU is enabled.

#### 3.2 Chosen approach: Static trampoline in `.text` + writable chain slot in `.data`

Each nested function whose address is taken gets a trampoline:

```asm
; In .text — trampoline for f1.f2:
; Thumb-2 encoding, 4 instructions + 2 data words = 16+8 = 24 bytes
__tramp_f1__f2:
    LDR   r10, [pc, #8]    ; r10 = *(PC+8) = chain_slot address
    LDR   r10, [r10]       ; r10 = *chain_slot = parent FP value
    LDR   pc, [pc, #4]     ; pc = *(PC+4) = f1__f2 address (tail call)
    NOP                     ; alignment padding (Thumb-2)
.Ltramp_f1__f2_func:
    .word f1__f2            ; R_ARM_ABS32 relocation to lifted function
.Ltramp_f1__f2_chain_ptr:
    .word __chain_slot_f1__f2  ; R_ARM_ABS32 reloc to .data slot

; In .data — writable slot:
__chain_slot_f1__f2:
    .word 0                 ; parent writes FP here at runtime
```

When the parent takes the address of the nested function:

```
// Pseudocode for generating IR when &f2 is referenced as a value:

function gen_addr_of_nested_func(nested_sym):
    // 1. Write current FP to the chain slot
    //    STR R7, [chain_slot_addr]
    emit IR: chain_slot_addr <- SYMBOL(__chain_slot_f1__f2)
    emit IR: STORE [chain_slot_addr], FP

    // 2. Return the trampoline address as the "function pointer"
    //    The caller will call __tramp_f1__f2 thinking it's a normal function
    emit IR: result <- SYMBOL(__tramp_f1__f2)
    return result
```

**Pseudocode for trampoline emission** (during the nested function's `gen_function` or a post-pass):

```
function emit_trampoline(nested_sym, parent_ir):
    // Save current output position
    saved_ind = ind

    // Emit Thumb-2 trampoline code:
    // All offsets relative to PC which is 4 bytes ahead in Thumb mode

    // LDR r10, [pc, #8]    — Thumb-2 T3 encoding
    emit_thumb32(0xF8DF, 0xA008)       // LDR.W r10, [pc, #8]

    // LDR r10, [r10, #0]   — dereference the chain slot pointer
    emit_thumb32(0xF8DA, 0xA000)       // LDR.W r10, [r10, #0]

    // LDR pc, [pc, #4]     — jump to the actual function
    emit_thumb32(0xF8DF, 0xF004)       // LDR.W pc, [pc, #4]

    // NOP for alignment
    emit_thumb16(0xBF00)               // NOP

    // Data words (with relocations):
    emit_word_with_reloc(nested_sym)   // R_ARM_ABS32 → f1__f2
    emit_word_with_reloc(chain_slot_sym)  // R_ARM_ABS32 → chain slot in .data

    // Create the chain slot in .data section
    chain_slot_sym = create_data_slot(".data", 4)  // 4-byte writable slot

    // Register trampoline symbol
    trampoline_sym = put_extern_sym_2(...)

    // Store trampoline info so parent can reference it
    nested_sym->trampoline_sym = trampoline_sym
    nested_sym->chain_slot_sym = chain_slot_sym
```

#### 3.3 Re-entrancy limitation

This approach is **NOT re-entrant**: if the parent function recurses, each recursive invocation writes the same `.data` chain slot. The last writer wins, corrupting earlier invocations' nested function pointers.

**Acceptable for now**: Most GCC torture tests don't combine recursion + nested function pointers. Document the limitation.

**Future fix**: Stack-allocated trampoline descriptors (Phase 3b, deferred):
- Allocate a `{func_addr, chain_value}` pair on the parent's stack
- Trampoline code in `.text` reads from a descriptor whose address is passed via R12 (IP)
- Requires an `alloca`-like mechanism or reserving stack space statically

#### 3.4 Detecting when address-of is needed

In `tccgen.c`, when a nested function symbol is used in a non-call context (i.e., its address is taken):

```
// Pseudocode in expression evaluation:

function handle_symbol_reference(sym):
    if sym is a nested function:
        if context is a direct function call (immediately followed by '('):
            // Direct call — no trampoline needed, just set up R10
            gen_call_nested_direct(sym, args)
        else:
            // Address taken — need trampoline
            sym->nested_addr_taken = 1
            gen_addr_of_nested_func(sym)
```

The `trampoline_needed` flag on the `NestedFunc` descriptor must be checked after the parent's `block(0)` to decide whether to emit a trampoline.

---

### Phase 4: IR Integration & Optimization Safety

**Effort**: 3-4 days
**Files**: `ir/core.c`, `ir/core.h`, `ir/codegen.c`, `ir/live.c`, `tccir.h`

#### 4.1 New fields on TCCIRState

```c
// tccir.h additions to TCCIRState:
typedef struct NestedFunc NestedFunc;  // forward decl

struct TCCIRState {
    ...existing fields...

    // Nested function support
    NestedFunc *nested_funcs;      // array of nested function descriptors
    int nb_nested_funcs;           // count
    int nested_funcs_capacity;     // allocated capacity

    uint8_t has_static_chain;      // 1 if this function is itself nested
    int static_chain_vreg;         // vreg holding R10 (chain pointer)
    int parent_loc;                // parent's `loc` value (for offset validation)
};
```

#### 4.2 Chain vreg as a parameter-like entity

The static chain register (R10) is modeled as a special parameter:

```
// Pseudocode for chain vreg initialization during nested gen_function:

function gen_function_nested_setup(ir):
    if not ir->has_static_chain: return

    // Allocate a vreg for the chain. It behaves like parameter but in R10.
    chain_vreg = tcc_ir_alloc_local_vreg(ir)
    ir->static_chain_vreg = chain_vreg

    // Mark in liveness: chain_vreg is live-in at instruction 0
    // Its live range spans the entire function (conservative)
    interval = find_or_create_interval(chain_vreg)
    interval->start = 0
    interval->end = ir->next_instruction_index  // updated at end
    interval->incoming_reg = REG_STATIC_CHAIN   // R10
    interval->addrtaken = 0  // it's a pointer, not an addressed var
```

#### 4.3 Optimization safety for captured variable accesses

Captured variable loads/stores go through the chain pointer (an indirection through R10). These must not be eliminated by:

- **Store-load forwarding**: Chain loads are through a different base register — the optimizer already treats different bases as distinct memory locations (no issue if using indexed LOAD/STORE with chain_vreg as base)
- **Dead store elimination**: A store through the chain modifies the parent's frame — it's externally visible. Mark chain stores as having side effects.
- **Constant propagation**: Cannot propagate through chain loads (the parent's memory could change between calls if the parent resumes)
- **CSE**: Chain loads from the same offset CAN be CSE'd within a basic block (the parent frame doesn't change while the nested function runs)

```
// Pseudocode: Mark chain-relative operations appropriately

function emit_chain_load(ir, dest_vreg, parent_offset):
    // Use regular LOAD but with chain_vreg as base
    src_op = make_operand_vreg_plus_offset(ir->static_chain_vreg, parent_offset)
    dest_op = make_operand_vreg(dest_vreg)
    tcc_ir_put_op(ir, TCCIR_OP_LOAD, src_op, NONE, dest_op)
    // No special flags needed — the load uses a non-FP base register,
    // so the optimizer already treats it as a memory access, not a stack local

function emit_chain_store(ir, parent_offset, src_vreg):
    dest_op = make_operand_vreg_plus_offset(ir->static_chain_vreg, parent_offset)
    src_op = make_operand_vreg(src_vreg)
    tcc_ir_put_op(ir, TCCIR_OP_STORE, src_op, NONE, dest_op)
    // Store through chain — the optimizer must not eliminate this
    // Since the base is a vreg (not FP), existing conservative rules apply
```

#### 4.4 Parent IR: chain setup before direct calls

When the parent calls a nested function directly, it must pass its FP in R10:

```
// Pseudocode for parent's call to nested function:

function gen_call_to_nested_func(ir, nested_sym, args):
    // Before the call, set R10 = current FP
    // This is modeled as: MOV R10, R7
    // In IR terms: allocate temp vreg, emit FP read, then a "call annotation"

    // Option A: Emit explicit ASSIGN from FP to a vreg assigned to R10
    tmp = alloc_temp_vreg()
    emit TCCIR_OP_ASSIGN tmp <- FP_OPERAND
    // The call instruction metadata records: R10 must hold `tmp` at call time
    emit TCCIR_OP_FUNCCALLVAL nested_sym, args, chain_vreg=tmp

    // Option B: Add a pre-call setup instruction
    emit TCCIR_OP_SET_CHAIN  (implicit: R10 <- FP)
    emit TCCIR_OP_FUNCCALLVAL nested_sym, args

    // Option B is simpler and avoids complex register constraints at call sites
```

---

### Phase 5: ARM Code Generation

**Effort**: 3-5 days
**Files**: `arm-thumb-gen.c`, `arm-thumb-opcodes.c`, `arm-thumb-opcodes.h`, `ir/codegen.c`

#### 5.1 Nested function prologue/epilogue

```
// Pseudocode for modified prologue generation:

function gen_func_prologue(ir):
    push_mask = compute_callee_saved_registers(ir)

    if ir->has_static_chain:
        // R10 must be saved (it's callee-saved anyway on ARM)
        push_mask |= (1 << 10)
        // R10 arrives pre-loaded with chain value
        // No additional setup needed — the chain vreg IS R10

    emit PUSH {push_mask}
    if need_frame_pointer:
        emit MOV R7, SP
    emit SUB SP, SP, #frame_size

function gen_func_epilogue(ir):
    // Standard epilogue — R10 restored from push
    emit ADD SP, SP, #frame_size
    emit POP {push_mask | (1 << PC)}   // or MOV PC, LR for leaf
```

#### 5.2 Chain-relative load/store codegen

```
// Pseudocode for lowering chain LOAD/STORE to Thumb-2:

function codegen_load_via_chain(ir, instruction):
    // Instruction: LOAD dest <- [chain_vreg + offset]
    // chain_vreg has been assigned to R10 by register allocator

    base_reg = get_physical_reg(instruction.src1)  // should be R10
    offset = instruction.offset
    dest_reg = get_physical_reg(instruction.dest)

    if offset fits in Thumb-2 LDR immediate (0..4095):
        emit LDR.W dest_reg, [base_reg, #offset]
    else:
        // Large offset — materialize in scratch
        scratch = get_scratch_register()
        emit_movw_movt(scratch, offset)
        emit LDR dest_reg, [base_reg, scratch]

function codegen_store_via_chain(ir, instruction):
    base_reg = get_physical_reg(instruction.dest_addr)  // R10
    offset = instruction.offset
    src_reg = get_physical_reg(instruction.src1)

    if offset fits in Thumb-2 STR immediate:
        emit STR.W src_reg, [base_reg, #offset]
    else:
        scratch = get_scratch_register()
        emit_movw_movt(scratch, offset)
        emit STR src_reg, [base_reg, scratch]
```

#### 5.3 `SET_CHAIN` instruction codegen (for parent calling nested func)

```
// Pseudocode for SET_CHAIN instruction lowering:

function codegen_set_chain(ir, instruction):
    // Emit: MOV R10, R7   (copy frame pointer to static chain register)
    // This is a Thumb-2 MOV register instruction
    emit_thumb16_mov(10, 7)   // MOV R10, R7
```

#### 5.4 Trampoline code emission

```
// Pseudocode for emitting trampoline after nested function is compiled:

function emit_trampoline_code(nested_sym, chain_slot_sym):
    // Emit into .text section, after the nested function's code

    // First, create the trampoline function symbol
    tramp_name = concat("__tramp_", nested_sym->name)
    tramp_start = ind

    // Thumb-2: LDR R10, [PC, #8]  — load address of chain slot
    //   PC at this point = tramp_start + 4 (Thumb pipeline)
    //   We want data at tramp_start + 16 (after 4 instructions × 4 bytes)
    //   Offset = 16 - 4 = 12... but actual Thumb-2 LDR literal encoding
    //   matters. Use proper opcode builder:
    arm_thumb_ldr_literal_w(R10, chain_ptr_offset)

    // Thumb-2: LDR R10, [R10, #0]  — dereference: r10 = *chain_slot
    arm_thumb_ldr_imm_w(R10, R10, 0)

    // Thumb-2: LDR PC, [PC, #offset]  — jump to nested function
    //   This loads the function address from the literal pool entry below
    arm_thumb_ldr_literal_w(PC, func_addr_offset)

    // Padding NOP if needed for alignment
    arm_thumb_nop()

    // Data: function address (with R_ARM_ABS32 relocation)
    emit_word(0)
    add_relocation(R_ARM_ABS32, nested_sym, ind - 4)

    // Data: chain slot address (with R_ARM_ABS32 relocation)
    emit_word(0)
    add_relocation(R_ARM_ABS32, chain_slot_sym, ind - 4)

    // Create & register trampoline symbol
    put_extern_sym_2(tramp_sym, cur_text_section, tramp_start + 1, ind - tramp_start, 0)
    //                                              +1 for Thumb bit

    // Store on nested func descriptor for the parent to reference
    nested_sym->trampoline_sym_index = tramp_sym->c
```

#### 5.5 Chain slot creation in `.data`

```
// Pseudocode:

function create_chain_slot(nested_sym):
    // Allocate 4 bytes in .data section
    data_sec = tcc_state->data_section  // or bss_section
    offset = section_add(data_sec, 4, 4)  // 4 bytes, 4-byte aligned

    // Create a symbol for it
    chain_slot_name = concat("__chain_", nested_sym->name)
    chain_slot_sym = put_elf_sym(...)

    // Initialize to 0
    write_word_at(data_sec, offset, 0)

    return chain_slot_sym
```

---

### Phase 6: Linker Support

**Effort**: 1-2 days
**Files**: `arm-link.c`, `tccelf.c`

#### 6.1 Relocations

The trampoline uses standard `R_ARM_ABS32` relocations for both the function address and chain slot address data words. No new relocation types needed.

```
// Pseudocode: Relocation handling (should work with existing code)

// In arm-link.c, relocate_section():
// R_ARM_ABS32 cases already handle:
//   *(uint32_t*)ptr += sym_addr
// This covers both:
//   .word f1__f2           → resolved to f1__f2's .text address (with +1 Thumb bit)
//   .word __chain_f1__f2   → resolved to chain slot's .data address
```

#### 6.2 Symbol visibility

Nested function symbols (`f1.f2` or `f1__f2`) should be `STB_LOCAL` in ELF — they are not externally visible:

```
// Pseudocode:

function create_nested_func_symbol(mangled_name, type):
    sym = external_sym(mangled_name_token, type, 0, &ad)
    // Force local binding — nested functions are not exported
    ELF32_ST_INFO(elfsym(sym)) = ELF32_ST_INFO(STB_LOCAL, STT_FUNC)
    return sym
```

Trampoline symbols (`__tramp_f1__f2`) and chain slot symbols (`__chain_f1__f2`) are also `STB_LOCAL`.

---

### Phase 7: Testing & Validation

**Effort**: 3-5 days
**Files**: `tests/ir_tests/`, `tests/gcctestsuite/conftest.py`

#### 7.1 Incremental test plan

| Test | Phase Required | What it validates |
|------|----------------|-------------------|
| `nested_basic.c` | 1 | Nested function def + direct call, no capture |
| `nested_capture_read.c` | 1+2 | Nested function reads parent variable via chain |
| `nested_capture_write.c` | 1+2 | Nested function writes parent variable via chain |
| `nested_direct_call_args.c` | 1+2 | Passing arguments + capturing parent vars |
| `nested_funcptr.c` | 1+2+3 | Address of nested function → trampoline |
| `nested_funcptr_indirect.c` | 1+2+3 | Nested func passed through another function (20000822-1 pattern) |
| `nested_multi_level.c` | 1+2 | Double-nested: f → g → h with capture |
| `nested_recursive_parent.c` | 1+2+3 | Recursive parent + nested function call |
| `20000822-1.c` | 1+2+3 | The original GCC torture test |

#### 7.2 Test: `nested_basic.c` (Phase 1 validation)

```c
// No capture, just direct call
int main() {
    int add1(int x) { return x + 1; }
    if (add1(41) != 42) abort();
    return 0;
}
```

Expected IR for `main`:
- Defines symbol `main.add1`
- `BL main.add1` with R10 = R7 (chain, unused by add1)

Expected IR for `main.add1`:
- Normal function, just happens to be nested
- No chain access, `has_static_chain = 0` (or 1 but unused)

#### 7.3 Test: `nested_capture_write.c` (Phase 2 validation)

```c
int main() {
    int x = 10;
    void set_x(int val) { x = val; }
    set_x(42);
    if (x != 42) abort();
    return 0;
}
```

Expected IR for `main.set_x`:
- `has_static_chain = 1`
- Loads chain pointer from R10
- Stores `val` to `[R10 + offset_of_x]`

#### 7.4 GCC torture test integration

```
// Pseudocode for conftest.py update:

// Remove skip entries for these 14 tests:
// 20000822-1.c, 920428-2.c, 920501-7.c, 920612-2.c, 921017-1.c,
// 921215-1.c, 931002-1.c, comp-goto-2.c, nestfunc-1.c, nestfunc-2.c,
// nestfunc-3.c, nestfunc-5.c, nestfunc-6.c, pr24135.c
//
// Keep comp-goto-2.c, nestfunc-5.c, nestfunc-6.c, pr24135.c skipped
// initially — they require computed goto / nonlocal goto extensions
```

---

## Dependency Graph

```
Phase 1  ──→  Parser: save nested func body as TokenString
              │        + compile after parent's block(0)
              │
Phase 2  ──→  Static chain: R10 convention, captured var access
              │        via pre-scan + chain vreg
              │
Phase 3  ──→  Trampolines: .text code + .data chain slot
              │        for address-of nested function
              │
Phase 4  ──→  IR: chain vreg management, optimization safety
              │
Phase 5  ──→  ARM codegen: prologue R10 save, chain load/store,
              │        trampoline emission, SET_CHAIN lowering
              │
Phase 6  ──→  Linker: R_ARM_ABS32 relocs (mostly existing)
              │
Phase 7  ──→  Testing: incremental + 14 GCC torture tests
```

In practice, Phases 1-5 are interleaved: you can't test Phase 1 without at least stub codegen (Phase 5), and Phase 2 needs IR support (Phase 4). The recommended implementation order:

1. **Phase 1 + Phase 4 (core) + Phase 5 (stub)**: Get `nested_basic.c` working (no capture)
2. **Phase 2 + Phase 4 (capture) + Phase 5 (chain codegen)**: Get `nested_capture_*.c` working
3. **Phase 3 + Phase 5 (trampoline) + Phase 6**: Get `20000822-1.c` working
4. **Phase 7**: Run full GCC torture suite

---

## Estimated Total Effort

| Phase | Effort | Cumulative |
|-------|--------|------------|
| 1: Parser (save + reparse) | 2-3 days | 3 days |
| 2: Static chain + capture | 3-5 days | 8 days |
| 3: Trampolines | 5-7 days | 15 days |
| 4: IR integration | 3-4 days | 19 days |
| 5: ARM codegen | 3-5 days | 24 days |
| 6: Linker | 1-2 days | 26 days |
| 7: Testing | 3-5 days | 31 days |

**Total: ~4-5 weeks** for full nested function support with trampolines.
**Milestone 1 (~1 week)**: Direct nested function calls, no capture (`nested_basic.c`).
**Milestone 2 (~2 weeks)**: Capture support (`nested_capture_*.c`).
**Milestone 3 (~3.5 weeks)**: Full trampoline support, `20000822-1.c` passes.
**Milestone 4 (~4.5 weeks)**: All applicable GCC torture tests passing.

---

## Risks & Open Questions

1. **Re-entrancy**: Static `.text` trampolines with `.data` chain slots are not re-entrant for recursive parent functions. Is this acceptable, or do we need `alloca`-based descriptors? (Acceptable for now — document limitation.)

2. **`gen_function()` calls `next()` at the end**: The reparse model via `begin_macro`/`end_macro` must correctly handle this. Verify that the token stream terminates cleanly after the `}` of the nested function body.

3. **Symbol mangling**: Names like `f1.f2` may conflict with C identifiers. Use `f1__nested__f2` or an internal-only token ID to avoid collisions.

4. **Nested-inside-nested**: Multi-level nesting (f → g → h) requires chasing chain pointers: `h` accesses `g`'s frame via its chain, and `g`'s chain to reach `f`. Each level adds one indirection. The chain vreg in `h` points to `g`'s frame, which contains `g`'s chain vreg pointing to `f`'s frame. Needs chain-of-chains support.

5. **Inline functions**: If a nested function is defined inside an inline function, the token-save method works naturally (inline expansion replays the outer tokens, which include the nested function save logic). But trampoline symbols need unique names per instantiation.

6. **`__label__` / nonlocal goto**: Tests `nestfunc-5.c`, `nestfunc-6.c`, and `pr24135.c` use nonlocal goto from nested functions. This requires stack unwinding support. Defer to a future phase.

7. **Optimization interaction**: Chain loads/stores must not be eliminated by store-load forwarding or dead store elimination. Since they use a non-FP base register (chain vreg → R10), existing conservative rules should suffice. Verify with test cases.

8. **Thread safety**: Static `.data` chain slots are not thread-safe. Acceptable for single-threaded embedded targets (Cortex-M33).

9. **Token pre-scan accuracy**: The `prescan_captured_vars` function does a shallow token scan — it cannot resolve scoping correctly (e.g., if the nested function declares a local with the same name as a parent variable, the pre-scan would over-mark). Conservative over-marking is safe (forces unnecessary stack spills) but suboptimal. Could refine later with a proper scope-aware scan.
