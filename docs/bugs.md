# Known bugs

## Bug: tccdebug SValue pointer marker on non-pointer base types

`tcc_debug_print_svalue()` prints a trailing `*` for `VT_LLONG` and other
non-pointer basic types whose numeric value shares bits with `VT_PTR`,
because it checks `if (vt & VT_PTR)` instead of testing the basic type with
`(vt & VT_BTYPE) == VT_PTR`. Confined to debug diagnostic output; no
compiler semantics affected. Not yet fixed.

## Bug: `tcc_set_linker()` boolean suboptions must be last in a `-Wl,` comma chain

`link_option()` (`libtcc.c:1268`): for a bare boolean flag (a `val` with no
`=`, e.g. `"Bsymbolic"`), the match loop requires `*p == '\0'` exactly
(`else if (*p) return 0;`) — it never special-cases a following comma. The
value-taking branch two lines above it (`if (*q == '=')`) explicitly
accepts `*p == ',' || *p == '='`. So `-Wl,-Bsymbolic,-rpath=/x` fails to
match `"Bsymbolic"` at all, falls through every `link_option()` check in
`tcc_set_linker()`'s if/else-if chain, and hits `unsupported linker option`
for the *entire* remaining chain even though every suboption is individually
valid. `-Wl,-rpath=/x,-Bsymbolic` (boolean flag last) or passing it alone
both work. Not yet fixed.

Likely fix: in the bare-boolean branch of `link_option()`, accept
`*p == ','` the same way the value-taking branch does, and have
`tcc_set_linker()`'s caller advance `option` past that comma (mirroring how
it already advances past a value via `skip_linker_arg`). Regression lock
(`tests/unit/arm/armv8m/test_libtcc_options_linker.c`,
`test_wl_boolean_flag_before_value_suboption_currently_fails`) pins the
current buggy behavior — flip its assertions once fixed.

## Bug: `_Pragma` operator is entirely unimplemented

`tccpp.c` has no handling anywhere for the `_Pragma(string-literal)` unary
operator required by C11 6.10.9 — only the `#pragma` *directive* form is
recognized, in `pragma_parse()` (`tccpp.c:2463`); there is no
`_Pragma`/`TOK__Pragma` keyword recognition in the lexer or `tccgen.c`'s
parser at all.

Per the standard, `_Pragma("X")` must be destringized and processed as if a
`#pragma X` directive appeared right there in the token stream — this is
what lets the common portable idiom `#define DO_PRAGMA(x) _Pragma(#x)`
conditionally emit pragmas from macros. Instead:
- Under `-E`, `_Pragma("message \"hi\"")` passes through completely
  untouched instead of being rewritten to `#pragma message "hi"` (verified
  against `gcc -E`, which does perform the rewrite).
- In a real (non-`-E`) compile, `_Pragma` is parsed as an ordinary,
  unrecognized identifier: at file scope this fails with `error: identifier
  expected`; inside a function body it produces `warning: implicit
  declaration of function '_Pragma'` followed by `error: ';' expected`. Any
  translation unit using `_Pragma` fails to compile outright.

Regression lock: `tests/frontend/pp/14_pragma_operator_currently_unsupported.c`
pins the `-E`-mode passthrough symptom. Once `_Pragma` support is added, its
golden (`14_pragma_operator_currently_unsupported.expect`) must be updated
to the destringized/rewritten form. Not yet fixed.

## Bugs: linker-script lexer over-eagerly swallows `.` and `*` as identifier characters

Root cause, three manifestations below: `ld_next_token()` in `tccld.c`
lists `.`, `*` (and, separately, never adds `!`) among the
identifier-*start* characters (`isalpha(c) || c=='_' || c=='.' ||
c=='*' || c=='$'`). A bare `.` or `*` in linker-script source is therefore
always lexed as `LDTOK_NAME` with `tok_buf == "."`/`"*"`, never as the raw
punctuation value — so every `if (p->tok == '.')` / `if (p->tok == '*')`
check elsewhere in the file is unreachable dead code. No fix attempted;
all three are pinned as regression tests documenting current behavior.

### linker-script location counter `.` is silently treated as a symbol named "."

`tccld.c`: `ld_next_token()` / `ld_parse_primary()` / `ld_parse_sections()` /
`ld_parse_output_section_contents()`

Because `.` never lexes as the raw char `'.'` (46), the location-counter
read in `ld_parse_primary()` and the location-counter *assignment* handling
in `ld_parse_sections()`/`ld_parse_output_section_contents()` never trigger.
`". = expr;"` falls through to the generic "symbol assignment" code path
and creates/updates a symbol literally named `"."`, while
`LDScript.location_counter` never advances via script content at all —
breaking address assignment, `"_end = .;"`-style epilogue symbols, and
`os->current_offset`/`start_lc` bookkeeping. Regression pin:
`tests/unit/arm/armv8m/test_ld_script.c`,
`test_bug_location_counter_dot_is_treated_as_phantom_symbol`.

### multiplication operator never applies in linker-script expressions

`tccld.c`: `ld_next_token()` / `ld_parse_mul()`

Same root cause: a standalone `*` (e.g. in `"2 * 3"`) lexes as
`LDTOK_NAME`, not the raw char `42`, so `ld_parse_mul()`'s
`while (p->tok == '*' || ...)` never fires: `"X * Y"` silently evaluates to
just `X`, and the unconsumed `"*"` token is picked up one level out and
misparsed as a brand-new top-level SECTIONS item (e.g. a bogus output
section literally named `"*"`, consuming the following number as its
address). No error is reported. Regression pin:
`tests/unit/arm/armv8m/test_ld_script.c`,
`test_bug_expr_multiplication_operator_never_applies`.

### malformed MEMORY attribute string causes silent phantom-region corruption

`tccld.c`: `ld_expect()` / `ld_parse_memory_attributes()` / `ld_parse_memory()`

`ld_expect()` does not advance the token position when it reports a
mismatch, and its return value is discarded by nearly every caller. The
`'!'` invert-attribute prefix (explicitly scaffolded for in
`ld_parse_memory_attributes()`'s char-switch) can never actually lex as
part of an identifier, since `!` is absent from both the identifier-start
and identifier-continuation sets. Once it appears, the parser gets stuck
re-reporting the same mismatch and falls into the generic "skip one token
and keep looping" fallback in `ld_parse_memory()`'s outer loop, which then
misinterprets leftover stray tokens (`rx`, `ORIGIN`, `LENGTH`, ...) as
brand-new memory-region names. Concretely,
`MEMORY { FLASH (!rx) : ORIGIN = 0x0, LENGTH = 1K }` silently produces 4
bogus regions (`FLASH`, `rx`, `ORIGIN`, `LENGTH`, all-zero fields) with an
overall `ld_script_parse_string()` return of 0 (success) — no crash, no
reported error. Regression pin: `tests/unit/arm/armv8m/test_ld_script.c`,
`test_bug_memory_invert_attribute_causes_phantom_regions`.

## Bug: linker-script section-pattern parsing leaves a bogus empty leading pattern entry

`tccld.c`: `ld_parse_section_pattern()`

Every call unconditionally adds one `LDSectionPattern` via
`ld_add_pattern()` *before* parsing the real glob name(s) inside the
parens (apparently meant to eventually capture a leading file-pattern,
e.g. the `*` in `*(.text*)`), but never populates that entry's `.pattern`
field. Every single `*(...)`/`KEEP(...)` occurrence therefore leaves one
permanent bogus entry (`pattern==""`, `type==LD_PAT_GLOB`, `keep` =
whatever was passed in), doubling `nb_patterns` and polluting
`ld_script_dump()` output. Harmless for `ld_section_should_keep()` today
(an empty pattern can't match a non-empty section name) but a real,
observable data-structure defect. Regression pin:
`tests/unit/arm/armv8m/test_ld_script.c`,
`test_sections_output_section_dotted_with_patterns_and_keep`. Not yet fixed.

## Bug: linker-script standard field order (`> REGION AT > LMA :PHDR`) silently drops the phdr association

`tccld.c`: `ld_parse_sections()`

The per-output-section suffix-clause parsing checks `'>'` (region), then
`':'` (phdr), then `"AT"` (load region) — in that fixed order, exactly once
each. Real-world scripts conventionally write
`"> REGION AT > LMA_REGION :PHDR"` (AT *before* the phdr tag); with that
ordering the `':'` check has already run (and seen `"AT"`, not `':'`) by
the time `AT > LMA_REGION` is consumed, and the trailing `:PHDR` is never
looked at again — `os->phdr_idx` silently stays `-1`, no error reported.
Only the non-standard `"> REGION :PHDR AT > LMA_REGION"` order works.
Regression pin: `tests/unit/arm/armv8m/test_ld_script.c`,
`test_bug_sections_standard_region_at_phdr_order_drops_phdr` (paired with
`test_sections_region_at_and_phdr_supported_order`, which shows the order
that does work). Not yet fixed.

## Bug: `tcc_opt_get_level()` can only return 0 or 1

`tccopt.c`: `tcc_opt_get_level()`

The function comment claims it "Map TCC's optimization settings to our
levels", but the implementation only inspects `tcc_state->opt_fp_offset_cache`.
It returns 1 whenever that flag is set and 0 otherwise; there is no code path
that returns 2 (or higher) to reflect `-O2`/`-O3`/`-Os`. Consequently, a caller
using this level to decide which passes to run will under-select optimizations
whenever the user requests `-O2` but the FP-offset-cache flag is off, or
over-select at `-O0` if the flag happens to be on. The real pipeline in
`ir/opt_pipeline.c` does not currently use this helper, so the bug is latent.
Regression pin: `tests/unit/arm/armv8m/test_tccopt.c`,
`test_opt_get_level_bug_comment_claims_map_but_only_reads_fp_cache`. Not yet
fixed.

## Bug: `tccelf_delete()` frees `sym_attrs` but leaves pointer/count stale

`tccelf.c`: `tccelf_delete()`

`tccelf_delete()` calls `tcc_free(s1->sym_attrs)` but does not reset
`s1->sym_attrs` to NULL or `s1->nb_sym_attrs` to 0.  If the same `TCCState`
is reused without being zeroed, a later `get_sym_attr(s1, index, 1)` sees
`index >= s1->nb_sym_attrs` as false (because `nb_sym_attrs` is still
non-zero), returns a pointer into the freed allocation, and writes to it.
The usual compiler teardown frees the whole `TCCState` immediately after
`tccelf_delete()`, so the bug is latent for normal usage, but it makes the
lifecycle contract unreliable for any caller that deletes ELF state and then
re-initializes the same state.

Regression pin: `tests/unit/arm/armv8m/test_tccelf.c`,
`test_tccelf_delete_leaves_sym_attrs_stale`. Not yet fixed.

## Bug: `dwarf_emit_reg_op()` / `dwarf_loc_reg_op_len()` silently accept negative register numbers

`tccdbg.c`: `dwarf_loc_reg_op_len()` (`tccdbg.c:2066`) and `dwarf_emit_reg_op()` (`tccdbg.c:2073`)

Both helpers check `regno >= 0 && regno <= 31` to decide whether to use the
short `DW_OP_reg0..DW_OP_reg31` form.  For negative `regno` values the check
fails, the value is then treated as an unsigned quantity, and a `DW_OP_regx`
location expression is emitted followed by a multi-byte ULEB128 encoding of
the (now huge) register number.  Negative register numbers are invalid in DWARF;
the function should either assert or report an error instead of silently
emitting nonsensical location information.

Regression pin: `tests/unit/arm/armv8m/test_tccdbg.c`,
`test_dwarf_emit_reg_op_negative_reg_encodes_as_regx` and
`test_dwarf_loc_reg_op_len_edge_cases`. Not yet fixed.


## Bug: `exact_log2p1()` mis-handles `0x80000000` due to signed `int` parameter

`tccgen.c`: `exact_log2p1()` (`tccgen.c:12596`)

The function takes an `int` parameter and loops while `i >= 256`, shifting right.
For the input `0x80000000` the argument becomes `INT_MIN`; the signed comparison
`i >= 256` is false and the loop exits immediately, so the function returns `1`
instead of `32`.  Any power-of-two value whose highest set bit is the sign bit
is affected.

Regression pin: `tests/unit/arm/armv8m/test_tccgen.c`,
`test_exact_log2p1_int_min_power_of_two` (currently documents the buggy
return value). Not yet fixed.

## Bug: `subst_asm_operand()` modifier `'n'` neither prefixes `#` nor negates the value

`arm-thumb-asm.c`: `subst_asm_operand()` (`arm-thumb-asm.c:223-226`)

GCC's `'n'` asm operand modifier means "immediate integer operand with a
known numeric value, negated".  The implementation in
`subst_asm_operand()` does compute `val = -val` when `modifier == 'n'`,
but then:

1. The leading `'#'` guard (`modifier != 'n'`) suppresses the `#` prefix
   for `'n'`, so the output is a bare number instead of an immediate.
2. The formatted output uses `sv->c.i` directly instead of the negated
   `val`, so the number is not negated either.

Result: for an input constant `42`, modifier `'n'` currently produces
`"42"` instead of the expected `"#-42"`.  Both the prefix and the sign
are wrong.

Regression pin: `tests/unit/arm/armv8m/test_arm_thumb_asm.c`,
`test_subst_const_n_modifier_known_bug`. Not yet fixed.

## Bug: `COND_NAMES_COUNT` is too small for the `cond_names[]` table

`arm-thumb-defs.h`: `#define COND_NAMES_COUNT 16` (`arm-thumb-defs.h:297`)
`arm-thumb-asm.c`: `cond_names[]` (`arm-thumb-asm.c:985-1004`)

The table contains 18 entries: 16 ordinary condition names (`eq` through
`al`), a `{NULL, 14}` terminator used as the default/unconditional case,
and one alias (`hs`/`cs` or `lo`/`cc` depending on ordering).  Because the
three loops over `cond_names` in `arm-thumb-asm.c` stop at
`COND_NAMES_COUNT` (16), the `"al"` entry and everything after it are never
consulted.  An explicit `addal`-style suffix therefore fails to match the
condition code and is not stripped from the mnemonic, even though the
assembler clearly intends `al` to be recognized (it is present in the
array and has a dedicated `COND_AL` code).

The mismatch also means the `{NULL, 14}` terminator is not reached by the
loops, so the documented default-to-`AL` behavior only happens because the
caller initializes `condition = COND_AL` before the loop, not because the
terminator matched.

Likely fix: set `COND_NAMES_COUNT` to the actual number of non-sentinel
entries (or derive it with `ARRAY_SIZE(cond_names)`), and ensure the loops
still stop before the `NULL` sentinel if the sentinel is meant to be a
default rather than a searchable name.

Not yet fixed.

## Bug: `tcc_tcov_end()` allocates terminator bytes but does not initialize them

`tccdbg.c`: `tcc_tcov_end()` (`tccdbg.c:3206-3214`)

When test-coverage output is enabled, `tcc_tcov_end()` appends one byte to
`tcov_section` for the last function name and one byte for the last file
name.  Those bytes are intended as terminators for the preceding
null-terminated strings, but `section_ptr_add()` only reserves the space;
it does not write a value to it.  The bytes therefore contain whatever was
already in the freshly allocated section memory, which is usually zero from
`section_realloc()` but is not guaranteed.  If the bytes are ever non-zero,
consumers that treat the coverage section as a sequence of
null-terminated strings will read past the intended end.

Likely fix: write `0` to the newly allocated byte(s), e.g.
`((char *)tcov_section->data)[tcov_section->data_offset - 1] = '\0';`
after each `section_ptr_add()`.

Regression pin: `tests/unit/arm/armv8m/test_tccdbg.c`,
`test_tcc_tcov_end_appends_terminators` (currently asserts only that the
section offset advanced; it does not assert the byte is zero). Not yet
fixed.
