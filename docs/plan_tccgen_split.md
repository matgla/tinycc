# Splitting `tccgen.c` — 31,434 lines → 52 files under `source/frontend/gen/`

> proposal · 2026-08-07 · refines
> [`restructure_architecture.md`](restructure_architecture.md) §6

§6 of the restructure proposal already maps `tccgen.c` onto ten `frontend/gen/*.c`
files. Ten is too coarse to call "small": that plan leaves `gen_expr.c` at ~6,500
lines and `gen_decl.c` at ~5,300 — bigger than `tccpp.c` (5,594) and `tccelf.c`
(7,254) respectively. This document keeps §6's location, its `gen_priv.h` linchpin,
and its extraction order, but cuts one level finer: **13 directories, 52 files,
median 507 lines, and only three files over 1,500.**

Every line range below is measured against the current `tccgen.c` (31,434 lines) and
partitions it exactly — no gaps beyond the license header, no overlaps.

## 1. Directory structure

```text
source/frontend/gen/
├── gen_priv.h              # THE linchpin — see §3
├── Makefile                # GEN_SRC / GEN_INC / GEN_HDRS, included by top-level
├── core/                   #   187  lifecycle, code suppression, type predicates
├── sym/                    # 1,021  symbol table, ELF symbols, attribute merge
├── value/                  # 2,098  vstack, register loading, long-long, string pool
├── type/                   # 1,920  compare/combine, sizes, casts, assign checks
├── op/                     # 3,724  arithmetic: int, float, complex, vector, folding
├── store/                  # 1,570  vstore + struct copy lowering
├── decl/                   # 3,234  attributes, structs, btype, declarators, decl()
├── expr/                   # 3,614  unary/primary, precedence parser, ternary, atomic
├── builtin/                # 6,719  __builtin_* families + call generation
├── inline/                 # 1,950  auto-inline analysis, const eval, emission
├── stmt/                   # 1,970  block, switch, cleanup, return
├── init/                   # 1,899  initializers, designators, storage allocation
└── nested/                 #   850  nested functions, trampolines, capture prescan
```

The root is `source/frontend/gen/` exactly as §3 of the restructure proposal
specifies, so this split is a step *into* that tree rather than a competing layout.
`tccpp.c` and `tccasm.c` move to `source/frontend/` later, unchanged.

## 2. File-by-file map

Line ranges are inclusive, against today's `tccgen.c`.

### `core/` — owns the globals, lifecycle, and the cheapest predicates

| File | Lines | Ranges | Contents |
|---|---:|---|---|
| `core/state.c` | 567 | 44–180, 424–436, 629–780, 972–1236 | Every `ST_DATA` definition, `initstr`, switch/temp-local/arg-struct state, `tccgen_init` / `tccgen_compile` / `tccgen_finish`, `tcc_bench_log_phase` |
| `core/suppress.c` | 64 | 782–845 | `gsym`, `gind`, `gjmp_acs`, `gjmp_addr_acs` and the `nocode_wanted` machinery |
| `core/predicates.c` | 123 | 848–970 | `is_float`, `is_integer_btype`, `btype_size`, `R_RET`, `PUT_R_RET`, `RC_RET`, `RC_TYPE`, `ieee_finite`, `test_lvalue`, `check_vstack` |

`core/state.c` is the only file that *defines* shared state; everything else reaches
it through `gen_priv.h`. The tiny predicates in `core/predicates.c` become
`static inline` in `gen_priv.h` instead — see §4 risk 1.

### `sym/` — cleanest boundary in the file, extract first

| File | Lines | Ranges | Contents |
|---|---:|---|---|
| `sym/symtab.c` | 451 | 1449–1899 | `__sym_malloc` pool, `sym_push`/`sym_pop`/`sym_find`/`sym_scope`, `struct_find`, `label_find`/`label_push`/`label_pop`, `token_stream_references_local_object` |
| `sym/elfsym.c` | 209 | 1238–1446 | `elfsym`, `update_storage`, `put_extern_sym2`, `put_extern_sym`, `greloca`, `greloc` |
| `sym/attr_merge.c` | 361 | 2584–2944 | `merge_symattr`/`merge_funcattr`/`merge_attr`, `patch_type`, `patch_storage`, `sym_copy`, `sym_copy_ref`, `external_sym`, the whole alias-resolution queue |

Exported surface: 31 functions, all of them already the natural API
(`sym_push` has 12 callers, `vpush_helper_func` 11, `elfsym` 8).

### `value/` — the vstack and its register machinery

| File | Lines | Ranges | Contents |
|---|---:|---|---|
| `value/vstack.c` | 308 | 1901–1955, 2195–2447 | `vcheck_cmp`, `vsetc`, `vswap`, `vpop`, `vpush*`, `vset`, `vseti`, `vpushv`, `vdup`, `vrotb`/`vrott`/`vrev`, `vset_VT_CMP`/`vset_VT_JMP`, `gvtst_set`, `gen_test_zero`, `vpushsym`, `get_sym_ref`, `vpush_ref` |
| `value/strlit_pool.c` | 134 | 2449–2582 | String-literal dedupe pool + `external_global_sym`, `external_helper_sym`, `vpush_helper_func` |
| `value/load.c` | 739 | 2946–3605, 3915–3993 | `get_temp_local_var`, `move_reg`, `gaddrof`, bounds checking (`gbound`, `gen_bounded_ptr_*`), packed bitfield load/store, `gv`, `gv2`, `gv_dup` |
| `value/longlong.c` | 917 | 3607–3914, 4007–4615 | `lexpand`, `lbuild`, `detect_ll_ext_provenance`, `try_emit_widening_mul64`, `gen_opl` |

### `type/` — the type system and conversions

| File | Lines | Ranges | Contents |
|---|---:|---|---|
| `type/compare.c` | 633 | 5312–5944 | `type_to_str`, `is_compatible_func`, transparent unions, `compare_types`, `compare_types_structural`, `combine_types`, `promote_bitfield_expr_type` |
| `type/size.c` | 189 | 10244–10390, 11500–11541 | `compute_aapcs_natural_alignment`, `type_size`, `vpush_type_size`, `pointed_type` |
| `type/cast.c` | 881 | 9362–10242 | `gen_cvt_itof1`, `force_charshort_cast`, `gen_cast_s`, `gen_cast_vector`, `gen_cast` |
| `type/assign_check.c` | 217 | 11542–11758 | `mark_value_bytes`, `mk_pointer`, `is_compatible_types`, `cast_error`, `verify_assign_cast`, `gen_assign_cast` |

`type/` has the widest export surface of any directory (20 functions; `type_size`
alone has 19 callers) — that is inherent, not a sign of a bad cut.

### `op/` — arithmetic lowering

| File | Lines | Ranges | Contents |
|---|---:|---|---|
| `op/op.c` | 340 | 7090–7429 | `gen_op`, `gen_op_impl` — the dispatcher (18 callers) |
| `op/int.c` | 338 | 4617–4954 | `value64`, `gen_opic_sdiv`, `gen_opic_lt`, `gen_opic` |
| `op/float.c` | 355 | 4956–5310 | `gen_negf` (both variants), `gen_opif` |
| `op/complex.c` | 1,143 | 5946–7088 | `gen_complex_int_cmp`, `gen_complex_float_cmp`, `gen_complex_int_arith`, `gen_complex_conjugate`, `gen_complex_float_arith`, `gen_complex_float_mul` |
| `op/vector.c` | 877 | 10392–11268 | SIMD: `is_vector_type`, `make_vector_type`, the vec-recipe cache, `gen_op_vector`, `gen_vec_subscript` |
| `op/fold_math.c` | 671 | 182–423, 437–628, 1957–2193 | `foldable_math_funcs[]`, `is_const_for_folding`, `get_const_double`/`float`, `inline_subst_const_arg`, `try_fold_math_call`, `try_fold_complex_call`, the objsize/`_chk` conservative-bound facts |

### `store/` — assignment lowering

| File | Lines | Ranges | Contents |
|---|---:|---|---|
| `store/vstore.c` | 1,340 | 11759–13098 | `vstore` — one function, 1,338 lines, 17 callers |
| `store/struct_copy.c` | 230 | 11270–11499 | `struct_has_vla_member`, `struct_has_bitfield_member`, `bitfield_unit_width`, `struct_member_copy_safe`, `ir_emit_struct_unit_copy` |

### `decl/` — declaration parsing

| File | Lines | Ranges | Contents |
|---|---:|---|---|
| `decl/attribute.c` | 432 | 13100–13531 | `inc`, `parse_mult_str`, `exact_log2p1`, `parse_c23_attribute`, `parse_attribute`, `parse_decl_attributes` |
| `decl/struct.c` | 733 | 13533–14265 | `find_field`, `check_fields`, `struct_layout`, `struct_decl` |
| `decl/btype.c` | 372 | 14267–14638 | `sym_to_attr`, `parse_btype_qualify`, `parse_btype` |
| `decl/predef_protos.c` | 190 | 14640–14829 | `tcc_predef_protos_table[]`, `tccgen_predef_sig_type`, `tccgen_predef_protos` |
| `decl/declarator.c` | 423 | 14831–15253 | `convert_parameter_type`, `parse_asm_str`, `asm_label_instr`, `post_type`, `type_decl` |
| `decl/decl.c` | 1,084 | 30351–31434 | `do_Static_assert`, `decl` — the top-level declaration loop |

### `expr/` — the expression grammar

| File | Lines | Ranges | Contents |
|---|---:|---|---|
| `expr/primary.c` | 1,870 | 22353–24222 | `unary_primary` |
| `expr/unary.c` | 487 | 22166–22352, 24223–24522 | `unary_paren`, `unary_generic`, `unary` |
| `expr/infix.c` | 328 | 24524–24712, 25082–25220 | `expr_prod`…`expr_or`, `precedence`, `init_prec`, `expr_infix`, `expr_cond`, `expr_eq`, `gexpr`, `expr_const`/`expr_const1`/`expr_const64` |
| `expr/cond.c` | 367 | 24714–25080 | `condition_3way`, `expr_landor`, `is_cond_bool`, `expr_cond_ternary` |
| `expr/indir.c` | 391 | 15255–15645 | `indir` (8 callers), `gfunc_param_typed`, `expr_type`, `parse_expr_type`, `parse_type`, `parse_builtin_params` |
| `expr/atomic.c` | 171 | 15647–15817 | `parse_atomic` |

### `builtin/` — the largest directory, and the one that benefits most

| File | Lines | Ranges | Contents |
|---|---:|---|---|
| `builtin/call.c` | 2,128 | 15963–16026, 16379–18442 | `gen_ir_void_call_args`, `nop_or_rollback_call_params`, `redirect_call_to_tcc_helper`, `unary_funcall` |
| `builtin/fp.c` | 938 | 18444–19286, 20023–20117 | `unary_builtin_alloca`, `unary_builtin_fp`, `unary_builtin_modf` |
| `builtin/fp2.c` | 734 | 19288–20021 | `unary_builtin_fp2` |
| `builtin/overflow.c` | 765 | 20119–20883 | `unary_builtin_overflow` |
| `builtin/simd.c` | 630 | 20885–21514 | `unary_builtin_shuffle`, `unary_builtin_convertvector` |
| `builtin/chk.c` | 648 | 21516–22163 | `unary_builtin_chk` (the `_FORTIFY_SOURCE` family) |
| `builtin/string.c` | 731 | 7431–7810, 16028–16378 | `try_get_constant_string`, `fold_builtin_strcmp/strncmp/memcmp/memchr`, `get_builtin_abs_info`, `gen_inline_abs_from_vtop`, `try_inline_builtin_call`, `unary_funcall_opt_string_builtins` |
| `builtin/misc.c` | 143 | 15819–15961 | `gcc_classify_type`, `gen_bitop1`, `gen_builtin_libcall`, `gen_ir_call_args` |

### `inline/` — the auto-inliner's front half

| File | Lines | Ranges | Contents |
|---|---:|---|---|
| `inline/analysis.c` | 941 | 7812–8752 | `auto_inline_type_ok`/`sig_ok`/`param_count`, the whole `inline_body_has_*` family, `nested_has_genuine_capture`, `nested_capture_is_read_only`, label hide/restore |
| `inline/const_eval.c` | 607 | 8754–9360 | `inline_eval_*`, `try_inline_const_eval`, `inline_arg_is_constant_like` |
| `inline/emit.c` | 402 | 29948–30349 | `ir_inline_stash_flush`, `erase_text_range`, `gen_late_reopt_functions`, `function_text_section`, `gen_inline_functions`, `free_inline_functions` |

### `stmt/` — statements

| File | Lines | Ranges | Contents |
|---|---:|---|---|
| `stmt/block.c` | 884 | 26311–27194 | VLA save/restore, `new_scope`/`prev_scope`/`leave_scope`, `lblock`, `block`, `block_1` |
| `stmt/switch.c` | 275 | 25732–26006 | `case_sort`, `switch_can_use_jump_table`, `tcc_ir_add_switch_table`, `gcase_jump_table`, `gcase`, `end_switch` |
| `stmt/cleanup.c` | 302 | 26008–26309 | `__attribute__((cleanup))`: `try_inline_cleanup_call`, `try_call_scope_cleanup`, `try_call_cleanup_goto`, `block_cleanup` |
| `stmt/ret.c` | 509 | 25222–25730 | `gfunc_return`, `check_func_return` |

### `init/` and `nested/`

| File | Lines | Ranges | Contents |
|---|---:|---|---|
| `init/initializer.c` | 865 | 27196–28060 | `skip_or_save_block`, `parse_init_elem`, `init_assert`, `init_putz`, `decl_design_*`, `decl_designator`, `init_putv`, `sso_swap_struct_init` |
| `init/alloc.c` | 1,034 | 28062–29095 | `decl_initializer`, `type_contains_pointer`, `decl_initializer_alloc` |
| `nested/nested.c` | 850 | 29097–29946 | `find_nested_func_by_sym`, `setup_nested_func_trampoline`, `emit_all_trampolines`, `compile_nested_functions`, `prescan_captured_vars`, `prescan_token_buf_for_captures`, `prescan_vla_param_captured_vars` |

## 3. `gen_priv.h` — the linchpin

Every `source/frontend/gen/**.c` starts with `#include "gen_priv.h"` and nothing
before it. The header must carry six things:

1. **`#define USING_GLOBALS` then `#include "tcc.h"`.** `tcc.h` `#undef`s
   `USING_GLOBALS` at its end (tcc.h:2996), so each TU must define it *before*
   including `tcc.h`. Putting both in `gen_priv.h` makes this unmissable — but it
   means `gen_priv.h` must be the first include, with no `tcc.h` ahead of it.
2. **The `ST_DATA` externs** — `vtop`, `_vstack`, `nocode_wanted`, `ind`, `loc`,
   `rsym`, `anon_sym`, the `*_stack` symbol chains, `func_vt`/`func_var`/`func_vc`,
   `funcname`, the shared `CType` singletons.
3. **The macros currently defined mid-file** (`tccgen.c` today relies on them being
   file-scope): `vstack`, `NODATA_WANTED`, `DATA_ONLY_WANTED`, `CODE_OFF`/`CODE_ON`,
   `NOEVAL_WANTED`, `CONST_WANTED`, `VT_SIZE_T`, `VT_PTRDIFF_T`, `STMT_EXPR`/
   `STMT_COMPOUND`, `MAX_TEMP_LOCAL_VARIABLE_NUMBER`, `precedence_parser`.
4. **The `gjmp` override.** Lines 845–847 do `#define gjmp_addr gjmp_addr_acs` /
   `#define gjmp gjmp_acs`, shadowing tcc.h's `ST_FUNC int gjmp(int)`, and lines
   31430–31431 `#undef` them so no other TU sees it. Moving these to `gen_priv.h`
   scopes the override to exactly the gen TUs, which is the intent — but it is a
   real hazard and belongs in a commented block, not buried in a list.
5. **Shared types**: `init_params` (10 users across `init/` and `value/`),
   `struct switch_t`, `struct temp_local_variable`, `FuncallScratch`,
   `PendingAliasDef`, plus a `#include "tcc_scope.h"` for `struct scope`.
6. **The cross-module prototypes** — the 200-odd functions §2's tables show as
   exported. `tccgen.c`'s existing forward-declaration block (lines 734–780) is the
   seed set; the measured export list per directory is in §2.

**Six statics become explicit shared state.** These are today's invisible
cross-boundary channels; each must be declared in `gen_priv.h` with a comment naming
its producer and consumer, not left as a bare `extern`:

| State | Producer | Consumer | Crosses |
|---|---|---|---|
| `pending_label_diff_plus`/`_minus` | `gen_opic` | `init_putv` | `op/int.c` → `init/initializer.c` |
| `aapcs_last_const_init`, `..._size` | `gfunc_param_typed` | `unary_funcall` | `expr/indir.c` → `builtin/call.c` |
| `arg_struct_temp_busy` | `get_arg_struct_temp` | `block` (save/restore) | `core/state.c` ↔ `stmt/block.c` |
| `cur_switch` | `block_1` | `gcase`, `end_switch`, `tccgen_finish` | `stmt/block.c` → `stmt/switch.c`, `core/` |
| `arr_temp_local_vars`, `nb_temp_local_vars` | `get_temp_local_var` | `compile_nested_functions`, `tccgen_finish` | `value/load.c` → `nested/`, `core/` |
| `initstr` | `parse_mult_str` | `decl_initializer`, `tccgen_finish` | `decl/attribute.c` → `init/`, `core/` |

`cur_scope`/`loop_scope`/`root_scope` are already non-static and already declared in
`tcc_scope.h` — no work needed, which is the proof that this pattern works here.

## 4. Risks

**1 — Losing intra-TU inlining is the one that can actually cost something.**
Today GCC sees all 31,434 lines at once and inlines every small `static` helper
freely. The project tracks `tcc` `.text` hard (2.42 → 1.41 MiB, per
`docs/` size work) and on-device compile latency harder. Splitting into 52 TUs
removes that opportunity for the hottest leaf helpers: `vpushi` (21 callers),
`vpop` (18), `vswap` (17), `vpushv` (16), `vdup` (11), `elfsym` (8),
`is_float` (10), `btype_size` (8), `pointed_type` (9), `is_integer_btype` (7).

Mitigation: those are all ≤10-line leaf functions. Define them as `static inline`
in `gen_priv.h` rather than moving them into a `.c` — that is what
`core/predicates.c` exists to feed. **Gate the merge on a `metrics/` `.text`
baseline diff and a `-bench` compile-time A/B**, not on `make test` alone.

**2 — `tests/unit/arm/armv8m/test_tccgen.c` `#include`s `tccgen.c`.** All 4,313
lines of that suite reach `tccgen.c`'s file-local statics (`try_fold_math_call`,
`auto_inline_sig_ok`, `inline_body_has_*`, `compare_types_structural`, …) precisely
because they share a TU. After the split those statics are `gen_priv.h`-declared
cross-TU functions, so the suite can link them normally — which is strictly better —
but `tests/unit/arm/armv8m/Makefile` (:354, :753–783) needs the `build_tccgen`
target rewritten to link 52 objects, and its stub layer will need to satisfy the
union of what all 52 reference. Budget real time for this; it is the largest single
piece of work in the split.

**3 — Two scripts hardcode the filename.** `scripts/coverage_tccgen.py` extracts
`*/tccgen.c` from lcov (:236) — change to `*/source/frontend/gen/*`.
`tests/selfhost/test_selfhost_compile.py` lists `"tccgen.c"` as one TU (:32); it
becomes 52 entries. That is an improvement: self-host miscompile bisect currently
has to bisect *inside* a 31k-line TU, and would get 52 natural buckets instead.

**4 — Header-dependency fan-out.** `tccgen.c` currently pulls 14 headers including
`ir/ssa.h`, `ir/regalloc.h`, `source/opt/include/opt_pipeline.h` and
`source/backend/arch/arm/arm_regalloc.h`. Only `inline/emit.c`, `nested/nested.c`
and `decl/decl.c` need the IR-pipeline and arch headers; the other 49 files need
`tccir.h` and `ir/core.h` only. Keeping that discipline (the restructure proposal's
"`gen_decl.c` is the only frontend file that sees the IR optimization pipeline") is
what makes the arch seam in §5 of that document reachable — so it is worth enforcing
in the `Makefile` include paths rather than trusting convention.

## 5. Sequencing

One directory per commit, `make test` plus the frontend and IR suites green at each
step. Order is by *decreasing* isolation, so the hard, state-entangled cuts happen
last when `gen_priv.h` has already proven itself:

1. `gen_priv.h` + `Makefile` scaffolding, `tccgen.c` reduced to `#include "gen_priv.h"`
   and left otherwise whole. Nothing moves; this commit only proves the header works.
2. `sym/` — 31 exports, all already API-shaped. No shared statics beyond the
   allocator pool.
3. `decl/predef_protos.c`, `builtin/misc.c`, `value/strlit_pool.c`,
   `store/struct_copy.c` — the small self-contained leaves, one commit.
4. `inline/` — §6 of the restructure proposal calls this out as clean, and the
   measurement agrees: the analysis helpers touch no shared state.
5. `builtin/` — big but shallow; each `unary_builtin_*` is a self-contained `switch`.
6. `op/`, then `type/`, then `value/` — increasing vstack entanglement.
7. `stmt/`, `init/`, `nested/`, `decl/`, `expr/`, `store/` — the `cur_switch`,
   `initstr`, `arg_struct_temp_busy` and `pending_label_diff_*` handshakes land here.
8. What remains is `core/`. Delete `tccgen.c`.

## 6. What this does not fix

Three files stay large because a single function dominates them:
`builtin/call.c` (2,128 — `unary_funcall` is 2,064 of it), `expr/primary.c`
(1,870 — `unary_primary` is all of it), `store/vstore.c` (1,340 — `vstore` is
1,338). Splitting those further means decomposing the functions, which is a
behaviour-changing refactor and must not be smuggled into a file-move commit. They
are the natural follow-up once the boundaries exist.
