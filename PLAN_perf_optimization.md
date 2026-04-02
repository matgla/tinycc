# TCC Compiler Performance Optimization Plan

## Goal
Cut compilation time for simple tests (like `01_hello_world`) by **2x** — from ~15.7M to ~7.8M instructions (matching the baseline of tests that don't include system headers).

## Current Profile Summary

| Metric | 01_hello_world | 30_function_call (baseline) |
|--------|---------------|---------------------------|
| Total instructions | 15,733,668 | 7,825,952 |
| Self cost (TCC) | 5,345,797 | 1,545,913 |

### Cost Breakdown by Phase (hello_world, self cost)

| Phase | Self Cost | % | Key Functions |
|-------|----------|---|---------------|
| Linker | 1,724K | 32.2% | find_elf_sym_with_hash (1,067K), elf_output_file (287K), relocate (66K), section_cache_insert (62K), build_got_entries (49K) |
| Preprocessor | 1,669K | 31.2% | preprocess (594K), parse_comment (489K), next (177K), next'2 (109K), parse_define (99K), macro_subst_tok (94K), tok_str_add2_spc (41K) |
| glibc (from TCC calls) | 1,508K | 28.2% | strcmp (666K), memcmp (127K), memcpy (104K), memset (42K), read (58K) |
| Memory allocator | 262K | 4.9% | tal_realloc (92K), _int_free (76K), realloc (41K) |
| Parser | 128K | 2.4% | decl (35K), svalue_to_iroperand (37K), struct_decl (19K), expr_infix (18K), unary_primary (18K) |
| Codegen | 43K | 0.8% | tcc_ir_codegen_generate (43K) |

### Root Cause: stdio.h Processing + Symbol Resolution

The 2x gap between hello_world and baseline comes from:
1. **Preprocessing stdio.h** (~4M inclusive): parsing comments, expanding macros, tokenizing all transitive includes
2. **Symbol resolution** (~2M inclusive): `printf`/`puts` references pull in more newlib archive members, each needing symbol lookup
3. **Additional linking work** (~1.5M): more relocations, more GOT entries, more sections

---

## Optimization Strategy (3 Phases)

### Phase 1: Preprocessor Fast Path (Target: ~1M savings)

#### 1a. memchr-based comment skipping
**File**: `tccpp.c:938-982` (`parse_comment`)

Current inner loop processes 2 chars/iteration checking for `\n`, `*`, `\`:
```c
for (;;) {
  c = *++p;
  if (c == '\n' || c == '*' || c == '\\') break;
  c = *++p;
  if (c == '\n' || c == '*' || c == '\\') break;
}
```

**Proposed**: Use a lookup table or batch scanning to skip large comment blocks faster. Since backslash-newline inside comments is rare, we can use a fast scan that only checks for `*` and `\n`:
- Scan for `*` using a tight byte loop (or memchr if buffer boundaries allow)
- Only when `*` found, check if next char is `/`
- Track `\n` count separately with a post-scan pass or during the fast loop

**Expected savings**: ~300-400K (60-80% of parse_comment's 489K)

#### 1b. Reduce preprocess() overhead
**File**: `tccpp.c` (`preprocess`, `skip`, `next`)

- Profile shows `preprocess` at 594K self cost — investigate if there are redundant token checks or excessive `next()` calls during directive processing
- Look at `skip()` which is called heavily during preprocessing — may have unnecessary overhead

**Expected savings**: ~200-300K

#### 1c. Token buffer pre-allocation
**File**: `tccpp.c` (`tok_str_add2_spc`, `tok_str_realloc`)

- `tal_realloc_impl` is 92K, `tok_str_add2_spc` is 41K
- Pre-size token buffers based on file size estimate
- Increase `TOKSTR_MAX_SIZE` initial allocation

**Expected savings**: ~50-100K

### Phase 2: Linker Optimization (Target: ~2-2.5M savings)

#### 2a. Inverted alacarte loading loop ⭐ HIGH IMPACT
**File**: `tccelf.c:5535-5635` (`tcc_load_alacarte`)

Current approach iterates ALL archive symbols (~7296 for newlib) on each pass, calling `find_elf_sym_with_hash()` for each to check if it's undefined in the main symtab. For hello world with ~10-20 undefined symbols vs 7296 archive symbols, this is extremely wasteful.

**Proposed**: Invert the loop — iterate undefined symbols from symtab, look them up in the archive:
1. Build a hash table from the archive symbol index (sym_names[] → member_offsets[])
2. Iterate only undefined symbols in symtab
3. For each undefined symbol, look up in archive hash table

This reduces iterations from O(nsyms_archive × passes) to O(nsyms_undefined × passes).

**Expected savings**: ~1-1.5M (most of find_elf_sym_with_hash's 1,067K + much of strcmp's 666K)

#### 2b. Better hash table initial sizing
**File**: `tccelf.c` (`rebuild_hash`)

- `rebuild_hash` shows up at 72K in minimal test — this is hash table resizing during symbol table growth
- Pre-size hash tables based on archive symbol count to avoid rebuild
- Currently doubles when `nb_hashed_syms > 2 * nbuckets` — start larger

**Expected savings**: ~50-100K

#### 2c. Reduce redundant ELF operations
**File**: `tccelf.c` (`elf_output_file`, `section_cache_insert`)

- `section_cache_insert` at 62K — analyze if it's doing redundant insertions
- `elf_output_file` at 287K self — look for unnecessary section iterations

**Expected savings**: ~100-200K

### Phase 3: Precompiled Headers (Target: ~4-5M savings) ⭐ ARCHITECTURAL

This is the highest-impact change and requires an architectural addition.

#### 3a. PCH format design
**Scope boundary for phase 3a (strictly preprocessor-first, parser state deferred):**

Serialize now:
- **Identifier tail of `table_ident`** created after `tccpp_new()` seeds keywords/special builtins (`tccpp.c:4810-4875`). Store only post-keyword identifiers, in token order, and recreate them with `tok_alloc()` on load so token IDs stay identical without serializing raw `TokenSym *` pointers.
- **Active macro end-state from `define_stack`** (`tccpp.c:1725-1771`, `2083-2175`): macro name token, `type.t` (`MACRO_OBJ`/`MACRO_FUNC`/`MACRO_JOIN`), formal argument tokens + `is_vaargs`, and replacement token stream in native `tok_str` word encoding. Do **not** serialize parser-owned `sym_struct` / `sym_identifier` links.
- **Replay stream for the covered root header**: a pre-tokenized stream of the tokens the parser would have seen after preprocessing, including embedded `TOK_LINENUM` records (`tccpp.c:1609-1613`). This lets later phases skip file I/O, lexing, comment parsing, and macro expansion while still reparsing declarations from tokens instead of serialized parser state.
- **Interleaved preprocessor side effects that matter to later parsing**, not just final values. In this fork the important one is `#pragma pack` (`tccpp.c:2281-2328`), so the replay stream must include explicit PACK_SET / PACK_PUSH / PACK_POP events at the original points. Final-only pack state is not enough because struct layout depends on mid-header transitions.
- **Include-cache snapshot** from `CachedInclude` / `cached_includes_hash` (`tcc.h:820-826`, `1073-1076`, `tccpp.c:2182-2228`) for every header transitively covered by the PCH: stored path string, `ifndef_macro`, `once`.
- **Other final preprocessor outputs** that affect later compilation: `#pragma comment(lib)` accumulated strings, and `__COUNTER__` consumption as a **delta** from entry (`pp_counter`, `tccpp.c:68`, `4447-4449`, `4797`).
- **Dependency + environment manifest**: root header path, normalized include path list, hash of the synthetic predefines text built in `preprocess_start()` (`tccpp.c:4756-4785`) plus `cmdline_defs` / `cmdline_incl`, and per-header stat info.

Defer to later phases:
- Parser/type state: `global_stack`, `local_stack`, typedefs, tags, enum constants, `sym_identifier`, `sym_struct`, anonymous symbol numbering, current function state (`tccgen.c`, `tcc.h`).
- Runtime preprocessor stacks that only make sense mid-expansion: `macro_stack`, `macro_ptr`, `unget_buf`, live `include_stack`, live `ifdef_stack`, partial conditional state.
- Full `#pragma push_macro` / `#pragma pop_macro` history across the PCH boundary. Phase 3b/3c should preserve only the active macro end-state and require headers included into a PCH to leave push/pop balanced.
- Rare pragma side effects that are hard to replay safely (`#pragma option` mutating compiler options mid-header). Phase 3b should reject PCH generation if such directives occur inside the covered header closure.

**Concrete file format (PCH v1):**
- Fixed header:
  - magic (`"TCPH"`), format version, header size, section count
  - target/build ABI tuple: endianness, `sizeof(int)`, `LONG_SIZE`, `LDOUBLE_SIZE`, pointer size, `TOK_IDENT`, hash of `tcc_keywords`
  - compiler/build fingerprint: `VERSION`, target/float ABI, file kind (`C` vs asm-preproc)
  - manifest hashes: predefines hash, include-path hash, dependency hash
  - counts: identifiers, macros, macro args, replay records, include records, pragma libs, dependencies
  - `counter_delta`
- Section directory with `(kind, offset, size)` entries so the file can be `mmap`ed / read-once, but loader still copies into TinyCC runtime structs rather than aliasing on-disk pointers.
- Sections:
  1. **STRINGS**: packed NUL-terminated strings for identifier names, filenames, pragma libs.
  2. **IDENTS**: ordered records `{string_off, len}` for identifiers allocated after the built-in keyword block.
  3. **TOKEN_BLOBS**: raw native `int` word token streams exactly as produced by `tok_str_add2()` / macro storage. This keeps 3b simple; 3c validates ABI tuple before use.
  4. **MACROS**: `{name_ident_index, macro_type, arg_start, arg_count, tok_blob_off_words, tok_blob_len_words}`.
  5. **MACRO_ARGS**: ordered `{ident_index, is_vaargs}` entries mirroring the `Sym->next` parameter chain used by `parse_define()`.
  6. **REPLAY_RECORDS**: ordered stream for one root include, each record being either `TOKENS(blob_off,len)`, `PACK_SET(value)`, `PACK_PUSH(value)`, `PACK_POP`, or `PRAGMA_LIB(string_off)` if we want replay ordering for diagnostics. Token records should preserve `TOK_LINENUM` values.
  7. **CACHED_INCLUDES**: `{filename_off, ifndef_ident_index_or_minus1, once}`.
  8. **DEPENDENCIES**: `{filename_off, size, mtime_sec, mtime_nsec}` for the root header and every transitive include actually opened during generation.
  9. **MANIFEST**: root header name, optional canonical path, maybe human-readable generator version.

**Invalidation / versioning strategy:**
- Hard reject on header/version mismatch: PCH magic/version, ABI tuple, target build fingerprint, or changed `tcc_keywords` hash.
- Hard reject if the hash of the predefines text differs. This automatically covers most preprocess-affecting `TCCState` bits (`cversion`, GNU/TCC extensions, ARM float ABI, `_REENTRANT`, etc.) because they change the synthetic `#define` stream emitted by `preprocess_start()`.
- Hard reject if normalized include path list or `-B`-derived TCC include location changes, because different search order can resolve the same `#include` text to different files.
- Hard reject if any dependency file path is missing or its `(size, mtime)` tuple changed. Phase 3a should specify this fast stat-based invalidation now; stronger content hashing can be a later upgrade if needed.
- PCH is only valid when loaded at a clean include boundary: no active macro expansion, no pending `unget_buf`, and not inside an unfinished conditional block.

**Likely entry points / files to edit for 3b/3c:**
- `tcc.h`:
  - add PCH manifest/section structs and new `TCCState` fields (`pch_infile`, `pch_outfile`, mode flags, loaded-manifest cache, maybe dependency vector).
  - expose `tcc_pch_generate()`, `tcc_pch_try_load()`, `tcc_pch_free()` prototypes.
- `libtcc.c` + `tcc.c`:
  - parse CLI/API options (`-generate-pch`, `-use-pch`, later maybe auto-sidecar mode).
  - route header-only PCH generation mode before normal compile.
- `tccpp.c`:
  - hook `preprocess_start()` to initialize PCH capture/load state.
  - hook include handling around the `search_cached_include` / include-open path (`tccpp.c:1837-1895`) so a matching PCH can inject a replay stream instead of opening/parsing the real header.
  - add helpers to enumerate active macros from `define_stack`, serialize token blobs, record pragma-pack events in `pragma_parse()`, snapshot `cached_includes`, and rehydrate them.
  - add a dedicated token-capture loop for generation that drives `next()` with parser-like flags (`PARSE_FLAG_PREPROCESS | PARSE_FLAG_TOK_NUM | PARSE_FLAG_TOK_STR`) and writes a root replay stream without invoking `tccgen_compile()`.
- Optional new `tccpch.c` / `tccpch.h` if `tccpp.c` grows too large; keep the actual preprocessor hooks in `tccpp.c` either way.

#### 3b. PCH generation
Add `-generate-pch <header>` (or equivalent explicit API) with this lifecycle:
1. Run `preprocess_start()` normally so builtins, `cmdline_defs`, and forced includes are already applied.
2. Open the requested root header exactly as normal include resolution would.
3. Drive a PCH capture loop that records:
   - emitted token stream chunks (with `TOK_LINENUM`)
   - `#pragma pack` events as replay records
   - final active macro table
   - `cached_includes` closure
   - pragma libs, dependency manifest, `counter_delta`
4. Serialize the snapshot to one PCH file; phase 3b should not attempt parser-state capture.
5. Refuse generation if unsupported mid-header side effects are seen (`#pragma option`, unbalanced push/pop macro state, non-C mode mismatches).

#### 3c. PCH loading
Add `-use-pch <file>` with this loader contract:
1. Validate the file header + manifest before touching live compiler state.
2. Recreate post-keyword identifiers in exact stored order via `tok_alloc()` and assert the resulting token numbers match the serialized identifier indexes.
3. Rebuild active macros with fresh `Sym` objects via `sym_push2(&define_stack, ...)`, repoint `table_ident[..]->sym_define`, restore pragma-lib list and `cached_includes`, and advance `pp_counter` by `counter_delta`.
4. On the matching `#include` site, inject the replay stream as a `TokenString`/macro-style stream (via `begin_macro()` on a non-owning buffer or copied token buffer) instead of opening the real header.
5. While replaying, execute PACK_* records at the original positions so parser-visible layout matches a real include.
6. After replay finishes, normal preprocessing continues; later duplicate includes are skipped by the restored `cached_includes` entries.
7. If validation fails, fall back to normal header processing without changing semantics.

#### 3d. Automatic PCH for common headers
- Build PCH for `<stdio.h>`, `<stdlib.h>`, `<string.h>` etc. during `make cross`
- Auto-detect and use when these headers are included
- Fall back to normal processing if PCH is stale

**Expected savings**: ~4-5M inclusive (eliminates all preprocessing, comment parsing, macro expansion for system headers)

---

## Implementation Order & Dependencies

```
Phase 1a (comment skip)  ─── no deps ──→  can start immediately
Phase 1b (preprocess)    ─── no deps ──→  can start immediately  
Phase 1c (token buffers) ─── no deps ──→  can start immediately
Phase 2a (inverted alacarte) ── no deps ──→  can start immediately
Phase 2b (hash sizing)   ─── no deps ──→  can start immediately
Phase 2c (ELF reduce)    ─── depends on 2a (measurement) ──→ after 2a
Phase 3a (PCH design)    ─── no deps ──→  can start immediately (design doc)
Phase 3b (PCH generation) ── depends on 3a ──→ after 3a
Phase 3c (PCH loading)    ── depends on 3a, 3b ──→ after 3b
Phase 3d (auto PCH)       ── depends on 3b, 3c ──→ after 3c
```

## Estimated Impact Summary

| Phase | Savings (instructions) | Cumulative Total | Improvement |
|-------|----------------------|-----------------|-------------|
| Baseline | — | 15,733,668 | 1.0x |
| Phase 1 (preprocessor) | ~1M | ~14.7M | 1.07x |
| Phase 2 (linker) | ~2-2.5M | ~12.2-12.7M | 1.24-1.29x |
| Phase 3 (PCH) | ~4-5M | ~7.7-8.7M | **1.8-2.0x** |

## Validation Strategy
- Use existing `callgrind` profiling (tests/ir_tests/profile_results/)
- Use `profile_compare.py` for before/after comparison
- Run `make test -j16` after each phase to ensure no regressions
- Benchmark across all 207 test profiles to ensure no test gets slower

## Risks & Considerations
- **PCH complexity**: Serializing parser state (types, symbols) is complex; start with preprocessor-only PCH
- **PCH invalidation**: Must detect when headers change and regenerate
- **Inverted alacarte**: Need to efficiently iterate undefined symbols; symtab doesn't have a separate undefined-symbol list
- **Comment optimization**: Must preserve correct line number tracking
- **Test coverage**: All optimizations must pass full test suite

## Notes
- Parser and codegen are NOT bottlenecks (<3.2% combined) — no optimization needed there
- Process startup/glibc overhead (~5M) is fixed and cannot be optimized
- The minimal test baseline (~7.8M) represents the irreducible cost of compilation + linking
