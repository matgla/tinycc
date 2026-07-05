# Known bugs

The bugs below are the ones **not yet fixed**. Each is deferred for a specific
reason (large feature, high regression risk, or no clean/testable fix), noted
in its entry. Their regression tests continue to pin the current behavior.

For the record, the following previously-listed bugs have been **fixed** (with
their regression tests flipped to assert the corrected behavior):

- `tcc_debug_print_svalue()` pointer marker — now tests `(vt & VT_BTYPE) == VT_PTR`.
- `tcc_set_linker()` boolean suboption before a comma in a `-Wl,` chain.
- linker-script section-pattern parsing no longer leaves a bogus empty leading
  pattern entry (`ld_parse_section_pattern()`).
- linker-script `> REGION AT > LMA :PHDR` field order no longer drops the phdr
  association (`ld_parse_sections()` suffix clauses parsed order-independently).
- `tcc_opt_get_level()` now maps `s->optimize` (the `-O<n>` level) to 0/1/2.
- `tccelf_delete()` now resets `sym_attrs`/`nb_sym_attrs`.
- `exact_log2p1()` now takes an `unsigned int` and handles `0x80000000`.
- `subst_asm_operand()` `'n'` modifier now emits `#-N`, and a negative symbol
  offset now renders as `#sym-N` (not `#sym+-N`).
- `tcc_tcov_end()` now NUL-initializes the appended terminator bytes.
- `svalue_get_conservative_max_u64()` negative-value guard now casts to
  `(int64_t)` so it actually fires.
- `tcc_object_type()` now validates `e_ident[EI_CLASS]` against `ELFCLASSW`.
- `thumb_build_it_mask()` now parenthesizes `tolower(pattern[i]) == 't'`.

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

Deferred: implementing the operator properly (lexer/preprocessor
destringization + token-stream re-injection) is a substantial preprocessor
feature, out of scope for the surgical-fix pass.

Regression lock: `tests/frontend/pp/14_pragma_operator_currently_unsupported.c`
pins the `-E`-mode passthrough symptom. Once `_Pragma` support is added, its
golden (`14_pragma_operator_currently_unsupported.expect`) must be updated
to the destringized/rewritten form.

## Bugs: linker-script lexer over-eagerly swallows `.` and `*` as identifier characters

Root cause, three manifestations below: `ld_next_token()` in `tccld.c`
lists `.`, `*` (and, separately, never adds `!`) among the
identifier-*start* characters (`isalpha(c) || c=='_' || c=='.' ||
c=='*' || c=='$'`). A bare `.` or `*` in linker-script source is therefore
always lexed as `LDTOK_NAME` with `tok_buf == "."`/`"*"`, never as the raw
punctuation value — so every `if (p->tok == '.')` / `if (p->tok == '*')`
check elsewhere in the file is unreachable dead code.

Deferred: fixing the lexer is genuinely delicate. `.` must still start
identifiers for section names (`.text`, `.data`), and `*` must still start
file-glob patterns (`*.o`), so a naive removal from the identifier-start set
breaks those; a peek-ahead fix that emits a bare `.`/`*` as raw punctuation
would *activate* half-written parser branches (e.g. the `if (p->tok == '.')`
output-section block in `ld_parse_sections()`) that were written against the
other lexer model and are not fully correct. All three are pinned as
regression tests documenting current behavior.

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

## Bug: `dwarf_emit_reg_op()` / `dwarf_loc_reg_op_len()` silently accept negative register numbers

`tccdbg.c`: `dwarf_loc_reg_op_len()` (`tccdbg.c:2066`) and `dwarf_emit_reg_op()` (`tccdbg.c:2073`)

Both helpers check `regno >= 0 && regno <= 31` to decide whether to use the
short `DW_OP_reg0..DW_OP_reg31` form.  For negative `regno` values the check
fails, the value is then treated as an unsigned quantity, and a `DW_OP_regx`
location expression is emitted followed by a multi-byte ULEB128 encoding of
the (now huge) register number.  Negative register numbers are invalid in DWARF;
the function should either assert or report an error instead of silently
emitting nonsensical location information.

Deferred: the documented fix (assert / hard error on a negative regno) is not
cleanly exercisable in the current unit-test harness, which has no
death/abort-test support; adding the assert would abort the very regression
tests that pin the `-1` behavior. The defect is purely latent (negative
register numbers do not occur for valid input).

Regression pin: `tests/unit/arm/armv8m/test_tccdbg.c`,
`test_dwarf_emit_reg_op_negative_reg_encodes_as_regx` and
`test_dwarf_loc_reg_op_len_edge_cases`.

## Bug: `COND_NAMES_COUNT` is too small for the `cond_names[]` table

`arm-thumb-defs.h`: `#define COND_NAMES_COUNT 16` (`arm-thumb-defs.h:297`)
`arm-thumb-asm.c`: `cond_names[]` (`arm-thumb-asm.c:985-1004`)

The table contains 18 entries: 16 ordinary condition names (`eq` through
`al`), a `{NULL, 14}` terminator used as the default/unconditional case,
and one alias (`hs`/`cs` or `lo`/`cc` depending on ordering).  Because the
three loops over `cond_names` in `arm-thumb-asm.c` stop at
`COND_NAMES_COUNT` (16), the `"al"` entry and everything after it are never
consulted.  An explicit `addal`-style suffix therefore fails to match the
condition code and is not stripped from the mnemonic.

Deferred: the "obvious" fix (bump `COND_NAMES_COUNT` to 17 so `"al"` is
searchable) introduces a real regression. `get_base_instruction_name()` /
`thumb_parse_token_suffix()` strip condition suffixes greedily, *before*
consulting the instruction table, so making `"al"` strippable causes real
base mnemonics that end in `"al"` — `smlal`, `umlal` — to be mis-split into
`"sml"`/`"uml"`, breaking their assembly via `asm_opcode()`. A correct fix
requires instruction-table-aware condition stripping (only strip a suffix
when the remaining base is itself a known mnemonic), which is a larger
refactor than a `#define` bump. Explicit `addal` is a marginal, redundant
form (`al` is the default); trading real instructions for it is not worth it.

Regression pin: `tests/unit/arm/armv8m/test_arm_thumb_asm.c`,
`test_token_suffix_explicit_al_condition` documents the current behavior.
