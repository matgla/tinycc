# Phase 1: Parser — Save Nested Function Bodies as Tokens

**Effort**: 2-3 days
**Files**: `tccgen.c`, `tcc.h`, `tccir.h`

## Overview

When `decl(VT_LOCAL)` encounters a function body `{`, instead of erroring, save the token stream via `skip_or_save_block()` and compile the nested function after the parent's `block(0)` completes. This reuses TCC's proven inline function model.

## TODO

- [x] Define `NestedFunc` struct in `tcc.h`
- [x] Add `nested_funcs` array + capacity fields to `TCCIRState` in `tccir.h`
- [x] Modify `decl()` in `tccgen.c`: replace error gate at line ~11393 with nested function save logic
- [x] Validate nested func parameters (same checks as file-scope path)
- [ ] Create mangled symbol name (e.g., `parent__nested__child`)
- [x] Push nested func symbol into `local_stack` so parent body can reference it
- [x] Call `skip_or_save_block(&nf->func_str)` to save body tokens
- [x] Implement `compile_nested_functions()` in `tccgen.c`
- [x] Define `ParentSavedState` struct for all globals that must be saved/restored
- [x] Save all ~20 globals before nested func compilation
- [x] For each `NestedFunc`: replay tokens via `begin_macro`/`end_macro`, call `gen_function()`
- [x] Restore all globals after nested func compilation
- [x] Insert `compile_nested_functions()` call in `gen_function()` after `block(0)`, before optimizations
- [x] Handle `ind` correctly — nested func code goes to `.text` at current `ind`, then parent's `ind` restored
- [x] Free `NestedFunc` token strings in `tcc_ir_free()`
- [ ] Test with `nested_basic.c` (no capture, direct call only)

## Data Structures

```c
// tcc.h — new struct
typedef struct NestedFunc {
    TokenString *func_str;        // saved token stream of function body
    Sym *sym;                     // function symbol in parent's local scope
    CType type;                   // full function type
    AttributeDef ad;              // function attributes
    int v;                        // token id (function name)
    char filename[256];           // source filename for error messages
} NestedFunc;

// tccir.h — additions to TCCIRState
//   NestedFunc *nested_funcs;
//   int nb_nested_funcs;
//   int nested_funcs_capacity;
```

## Pseudocode: Modify `decl(VT_LOCAL)`

```
function decl(l):
    ...existing type parsing...

    if tok == '{':
        if l == VT_LOCAL:
            // ── nested function definition ──
            assert (type.t & VT_BTYPE) == VT_FUNC

            // Validate parameters (same as file-scope path)
            foreach param in type.ref->next:
                if param has no identifier: error("expected identifier")
                if param is void: param.type = int_type

            merge_funcattr(&type.ref->f, &ad.f)

            // Create mangled symbol: "parent__nested__child"
            mangled_name = concat(funcname, "__nested__", get_tok_str(v))

            // Push symbol into LOCAL scope so parent body can reference it
            type.t &= ~VT_EXTERN
            sym = sym_push(v, &type, VT_CONST, 0)  // VT_CONST: it's a function
            put_extern_sym(sym, cur_text_section, 0, 0)  // placeholder address

            // Save the token stream
            ir = tcc_state->ir
            grow_nested_funcs_if_needed(ir)
            nf = &ir->nested_funcs[ir->nb_nested_funcs++]
            nf->sym = sym
            nf->type = type
            nf->ad = ad
            nf->v = v
            strcpy(nf->filename, file->filename)
            skip_or_save_block(&nf->func_str)  // saves '{' ... '}'

            break  // continue parsing parent body
        else:
            // existing file-scope path (unchanged)
            ...
```

## Pseudocode: `compile_nested_functions()`

```
function compile_nested_functions(parent_ir, parent_sym):
    // Save ALL parent global state
    saved = ParentSavedState {
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

    for each nf in parent_ir->nested_funcs:
        // Replay saved token stream (same as inline function expansion)
        tccpp_putfile(nf->filename)
        begin_macro(nf->func_str, 1)
        next()  // prime the first token

        cur_text_section = saved.cur_text_section
        gen_function(nf->sym)
        end_macro()

    // Restore ALL parent state
    tcc_state->ir       = saved.ir
    loc                 = saved.loc
    // NOTE: do NOT restore ind — nested func code is in .text and
    // the parent's codegen will emit at the CURRENT ind (after nested funcs)
    // Actually: we DO restore ind. The parent's IR codegen emits code later
    // during tcc_ir_codegen_generate(), which sets ind itself.
    // Wait — gen_function() for the nested func modifies ind (it writes code).
    // The parent needs ind to continue where IT left off... but the parent
    // hasn't emitted code yet (we're before parent's optimization/codegen).
    // So nested func code goes at the current ind, and the parent will emit
    // its code at the NEW ind after all nested funcs.
    // DECISION: Do NOT restore ind. Let nested funcs claim their .text space.
    rsym                = saved.rsym
    func_ind            = saved.func_ind
    funcname            = saved.funcname
    func_vt             = saved.func_vt
    func_var            = saved.func_var
    cur_scope           = saved.cur_scope
    root_scope          = saved.root_scope
    loop_scope          = saved.loop_scope
    local_stack         = saved.local_stack
    local_label_stack   = saved.local_label_stack
    global_label_stack  = saved.global_label_stack
    nocode_wanted       = saved.nocode_wanted
    local_scope         = saved.local_scope
    nb_temp_local_vars  = saved.nb_temp_local_vars
    cur_text_section    = saved.cur_text_section
    cur_switch          = saved.cur_switch
    memcpy(arr_temp_local_vars, saved.arr_temp_local_vars, sizeof arr_temp_local_vars)
```

### Key detail: `ind` handling

`gen_function()` writes machine code at `ind` via `tcc_ir_codegen_generate()`. The nested function's code is written first (it runs `gen_function` end-to-end, including codegen). Then the parent resumes its own IR pipeline. The parent's `tcc_ir_codegen_generate()` will write code at the new `ind` (after nested funcs). So we do NOT restore `ind`.

But we DO need to restore `func_ind` — this tracks the START of the parent function in `.text` (used for symbol size calculation: `elfsym(sym)->st_size = ind - func_ind`).

## Pseudocode: Integration point in `gen_function()`

```
function gen_function(sym):
    ...existing setup (ir = tcc_ir_alloc(), params, etc.)...

    block(0)
    tcc_ir_backpatch_to_here(ir, rsym)

    // ── NEW: compile nested functions ──
    if ir->nb_nested_funcs > 0:
        compile_nested_functions(ir, sym)

    // ...existing optimization passes (operate on parent's ir)...
    // ...register allocation...
    // ...tcc_ir_codegen_generate(ir) — parent's code emitted AFTER nested funcs...
    // ...tcc_ir_free(ir)...
```

## Symbol Visibility

After `skip_or_save_block`, the nested function's `Sym` is on `local_stack`. When the parent body references `f2`, `sym_find()` resolves it to a function symbol just like any external function. Direct calls work with no special handling.

## Test Cases (Phase 1)

See [tests/nested_basic.c](tests/nested_basic.c), [tests/nested_basic_args.c](tests/nested_basic_args.c), [tests/nested_multiple.c](tests/nested_multiple.c).
