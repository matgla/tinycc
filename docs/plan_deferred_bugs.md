# Implementation plans for the deferred `docs/bugs.md` entries

These four bugs were deliberately **not** fixed in the surgical bug-fix pass
(each `docs/bugs.md` entry says why). This document is the concrete plan to fix
each one properly. Order below is roughly easiest → hardest.

Shared conventions:
- Every fix must **flip its pinned regression test** from "documents current
  behavior" to "asserts corrected behavior" (see each section).
- Build/run the affected unit-test binary from
  `tests/unit/arm/armv8m/` (`make run`, `run-tccpp`, etc.).
- No production source may be broken for the parallel test-writing work — keep
  changes surgical.

## 3. `_Pragma` operator entirely unimplemented (C11 6.10.9)

**STATUS: DONE.** `TOK__Pragma` added to `tcctok.h`; recognized in `next()`
(after macro expansion) and dispatched by `handle_pragma_operator()` in
`tccpp.c` — under `-E` it rewrites to `#pragma <text>` on its own line, on a
real compile it feeds `<text>` through the existing `pragma_parse()` machinery
so `pack()`/`push_macro`/etc. take effect. Tests: `tests/frontend/pp/`
`14_pragma_operator` (-E) + `19_pragma_operator_macro` (DO_PRAGMA idiom);
`tests/frontend/diagnostics/14_pragma_operator_bad` (malformed operand);
`tests/ir_tests/343_pragma_operator.c` (executable pack-layout end-to-end);
`test_tccpp.c` unit tests (`test_pragma_operator_*`). Note: making these unit
tests work required the harness's `tcc_open_bf`/`tcc_close` stubs in
`tccpp_stubs.c` to implement real buffer push/pop (they were no-ops).

**Files:** `tccpp.c` (lexer keyword table, `pragma_parse` ~2463, the
`next()`/`next_nomacro()` token path, `unget_tok`/`begin_macro` reinjection),
possibly `tcc.h`/`tcctok.h` for a `TOK__Pragma` id.
**Test binary:** compiler path — `tests/frontend/pp/14_pragma_operator.c`

### Semantics to implement
`_Pragma("string-literal")` is a unary operator usable anywhere in the token
stream (including produced by macro expansion). It must: parse `_Pragma`, `(`,
one string-literal, `)`; **destringize** the string (strip the quotes; replace
`\"`→`"` and `\\`→`\`); then process the result exactly as if `#pragma <text>`
had appeared there — i.e. re-lex the destringized text as a directive and run
the existing `pragma_parse()` machinery. Under `-E` it must be rewritten to a
`#pragma <text>` line (matching `gcc -E`).

### Approach
1. **Recognize the keyword.** Add `TOK__Pragma` to the token table (alongside
   the other preprocessor keywords). `_Pragma` must be recognized *after* macro
   expansion, so intercept it in the macro-expanding token path (`next()`), not
   only `next_nomacro()` — this is what lets `#define DO_PRAGMA(x) _Pragma(#x)`
   work.
2. **Parse the operand.** On seeing `TOK__Pragma`, read `(` `TOK_STR` `)` via
   `next_nomacro()` (like the `push_macro`/`pop_macro` arm of `pragma_parse`,
   `tccpp.c:2471-2477`). Reuse `tokc.str` for the literal.
3. **Destringize** into a `CString`: drop surrounding quotes, unescape `\"` and
   `\\`. (C11 §6.10.9p1.)
4. **Re-lex + dispatch.** Build a token string from the destringized text and
   feed it through the pragma path. Two options:
   - Wrap it as `# pragma <tokens> \n` and push it via `begin_macro()`/an
     `unget_tok` sequence so the normal directive handler runs it — symmetric
     with the existing `-E` passthrough (`unget_tok('#'); unget_tok(TOK_PRAGMA);`
     at `tccpp.c:2510-2513`); or
   - Tokenize the text into a `TokenString` and call the relevant body of
     `pragma_parse()` directly.
   Prefer routing through the existing `#pragma` dispatch so `pack`, `once`,
   `push_macro`, `comment`, etc. all work uniformly.
5. **`-E` mode.** When `output_type == TCC_OUTPUT_PREPROCESS`, emit
   `#pragma <destringized text>` on its own line instead of the raw
   `_Pragma(...)` (reuse the existing passthrough shape).

### Edge cases
- `_Pragma` from macro expansion (the whole point) — verify via
  `#define DO_PRAGMA(x) _Pragma(#x)`.
- Wide/`u8` string literals as the operand (reject or handle per standard).
- Malformed operand (missing `(`, non-string, missing `)`) → clear error.
- Nested inside expressions — since it's handled in the token stream it should
  disappear before the parser sees it.

### Test changes
- Update `tests/frontend/pp/14_pragma_operator_currently_unsupported.c` +
  its `.expect` golden to the destringized/rewritten `#pragma ...` form
  (verify against `gcc -E`), and rename to drop `currently_unsupported`.
- Add positive cases: `_Pragma("pack(1)")` affects struct layout like
  `#pragma pack(1)`; the `DO_PRAGMA` macro idiom; `-E` rewrite.

### Effort
~1 day. Largest of the four — real preprocessor feature with macro-expansion
interaction. Study how GCC/upstream tinycc route `_Pragma` before implementing.

---

## 4. Linker-script lexer swallows `.` / `*` / `!` (three manifestations)

**Files:** `tccld.c` (`ld_next_token` ~119, `ld_parse_primary` ~310,
`ld_parse_mul` ~441, `ld_parse_sections` ~860, `ld_parse_output_section_contents`
~742, `ld_expect` ~257, `ld_parse_memory_attributes` ~536, `ld_parse_memory`)
**Test binary:** main, `test_ld_script.c`

### Root cause
`ld_next_token()` lists `.` and `*` among identifier-**start** characters
(`isalpha||'_'||'.'||'*'||'$'`) and never lexes `!`. So a bare `.`/`*` always
becomes `LDTOK_NAME`, making every `p->tok == '.'` / `== '*'` check dead code,
and `!` can never appear as punctuation.

### Why it's the hardest
`.` must still start section names (`.text`, `.data`); `*` must still start file
globs (`*.o`). And several parser branches were written against a lexer that
*does* emit bare `.`/`*` (e.g. the whole `if (p->tok == '.')` output-section
block in `ld_parse_sections`, `tccld.c:870`), so fixing the lexer **activates
half-written code** that must be finished and verified.

### Approach — context-sensitive lexing + finish the activated branches
Do this in **three independent, separately-committable steps**, easiest first:

**Step A — `!` (contained, low risk).**
1. In `ld_next_token`, before the identifier block, emit `!` as a raw
   punctuation token (it is unused elsewhere, so no dead-code activation).
2. Make `ld_expect()` **advance** past a token on mismatch (or have callers do
   so) so a stray token can't wedge the loop; audit the discarded return values
   in `ld_parse_memory`.
3. Handle `!` in `ld_parse_memory_attributes`' char-switch (it already scaffolds
   for the invert prefix). Verify
   `MEMORY { FLASH (!rx) : ORIGIN=…, LENGTH=… }` yields **one** region with the
   inverted attribute, not four phantom regions.
   - Flip `test_bug_memory_invert_attribute_causes_phantom_regions`.

**Step B — bare `*` multiply operator.**
1. In `ld_next_token`, when `c == '*'`, peek: if the next char is an
   identifier-continuation char (so `*.o`, `*(...)` file globs still lex as one
   NAME as today — check what the glob paths actually require), keep the NAME
   behavior; otherwise emit raw `'*'`.  Concretely: emit raw `*` when the next
   non-space char is a digit/`(`/operator, so `2 * 3` works while `*(.text)` and
   `*.o` keep working (they are consumed by `ld_parse_section_pattern`, which
   already accepts both `LDTOK_NAME` and `'*'`).
2. Confirm `ld_parse_mul()`'s `while (p->tok == '*' …)` now fires.
   - Flip `test_bug_expr_multiplication_operator_never_applies`; re-run every
     `*(...)`/glob test (`test_sections_output_section_dotted_with_patterns_and_keep`,
     the pattern-match tests) to prove no glob regressed.

**Step C — bare `.` location counter (activates dead code).**
1. In `ld_next_token`, when `c == '.'`, peek: if the next char is an
   identifier-continuation char, keep the NAME behavior (`.text` stays one
   token); otherwise emit raw `'.'`.
2. **Finish the activated branches** that assumed bare `.`:
   `ld_parse_primary` (location-counter read), `ld_parse_output_section_contents`
   (`. = expr;`), and the `if (p->tok == '.')` block in `ld_parse_sections`
   (which currently mixes "output section starting with ." with the bare-`.`
   case — reconcile it with the fact that `.text` still arrives as one NAME via
   the `LDTOK_NAME` branch). Ensure `LDScript.location_counter`,
   `os->current_offset`, and `start_lc` are updated.
3. Verify `_end = .;`-style epilogue symbols and `. = ALIGN(4);` work.
   - Flip `test_bug_location_counter_dot_is_treated_as_phantom_symbol`.

### Cross-cutting test strategy
- After **each** step, run the **entire** `test_ld_script.c` suite — the risk is
  regressing a currently-passing script test, not just the flipped pin.
- Consider adding an end-to-end pin: parse a realistic CMSIS-style linker script
  (MEMORY + SECTIONS with `.text`/`.data`/`.bss`, `> FLASH AT > FLASH`, `_end = .;`)
  and assert region/offset/symbol results.

### Effort
~1 day across the three steps. Step A is a safe standalone win; Steps B and C
carry real regression risk (globs, section-name lexing, activated dead code) and
must land behind a full green `test_ld_script.c` run each.

---

## Suggested sequencing

1. **#1 dwarf** (30 min, isolated) and **#4 Step A `!`** (contained) — quick wins.
2. **#2 COND_NAMES_COUNT** — medium, with the `smlal`/`umlal` pins as the net.
3. **#4 Steps B/C** — one at a time, full suite green between each.
4. **#3 `_Pragma`** — the big feature; do last, study upstream first.
