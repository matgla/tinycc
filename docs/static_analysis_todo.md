# Static analysis: what tcc has and what is missing

The checks a safe-by-default tcc should have, compared with gcc (`-Wall
-Wextra`, `-fanalyzer`), clang (`-Wall`, `--analyze`) and Cake's flow
analysis.  libc's half (header annotations, runtime hardening) is in
`libs/libc/docs/static_analysis_todo.md`.

Finished items are one line each here.  Their full notes (design, traps,
measurements) are in this file's history: `git show b4750de56c:docs/static_analysis_todo.md`.
Open work: "Open items" and "Next" below.

Rules for every item:

- One `-W<name>` per check, in `options_W` (`source/driver/libtcc.c`). Put it
  in `WD_ALL` when it is cheap and has few false positives. Add it to
  `tcc_safe_defaults`' `as_errors` only when real code never trips it by accident.
- `-ccompat -O0` keeps the fast path (`ra_o0_direct`, no SSA). Only a check
  that is on may cost compile time.
- A check never changes code: objects at `-O1`/`-O2`/`-Os` stay byte-identical
  with and without it (see `tests/ir_tests/test_o0_lifetime_ssa.py`).
- Each check gets a `tests/ir_tests/test_<check>.py` covering hit, no hit,
  `-Wno-`, `-Wno-error=` and `-ccompat`.
- Results must not depend on the `-O` level. Flow checks run on the raw IR,
  right after the frontend finishes a function and before any pass
  (`source/ir/lifetime.c`). A check that needs proven values runs after
  optimisation and may differ by level: only `-Wnull-dereference`
  (`tcc_ir_null_deref_check`).
- A frontend warning is reported once per place however often a body is
  parsed (inline replays, check-only parses): `tcc_diag_once()` and its
  wrappers (`tcc_warning_once`, `tcc_fe_warning`, `tcc_diag_first`).
- Warnings are quiet in system headers and in the expansion of a macro a
  system header defines, unless `-Wsystem-headers`.

## Done

### Safe default and the yasc dialect

- [x] Safe by default: `-Wall`, the `WD_SAFE` options, six unsafe-C warnings
  as errors, the yasc dialect, `-Wlifetime`.  `-ccompat` (or `-std=c*`/`gnu*`)
  turns all of it off; the keywords are identifiers there.
- [x] Plain pointers are forbidden in yasc: every `*` says `owned`, `view` or
  `borrows(...)`; `maybe`, `out`, `drop` combine.  Function pointers exempt.
- [x] A parameter declared as an array: the pointer it becomes is a `view`
  (`char * view a[]` is `char * view * view a`; unchecked in regular C).  As
  C99's `a[const]`, the outermost brackets qualify it: `a[owned]`,
  `a[maybe]`, `a[view]`, `a[restrict]`.  Test:
  test_yasc_array_param.py.
- [x] `T * readonly p` (and `a[readonly]`) was `const T * view p`; removed
  2026-11-25 once a view parameter's target became read-only by default
  (in / inout / out): in a parameter it said nothing, elsewhere it spelled
  `const T * view` twice.  After a `*` (or in a parameter's brackets) it is
  an error naming the long form; everywhere else a name.  The plain-pointer
  fix-it gives a local copying an `in` parameter `char const * view p` (only
  the first declarator of a list: `, const * view b` is no C).  The C header
  writes an `in` parameter's `const` (test_yasc_modules.py
  test_header_spells_in_as_const).  Test: test_yasc_readonly.py.
- [x] The dialect is per file: the main file under `-std=yasc`, a file from
  its `#pragma yasc` on, headers under `-iyasc DIR` / `YASC_INCLUDE_PATH`.
  A regular-C macro's expansion in yasc code stays regular C (`tcc -E` marks
  it with `#pragma tcc c_begin` / `c_end`).  Tests: test_yasc_include_path.py,
  test_yasc_c_macros.py.
- [x] A pointer declared in regular C is `unchecked` from yasc code
  (`YASC_UNCHECKED`, docs/yasc_modules.md "Legacy C"): it converts to and from
  owned and view pointers silently.  GCC attributes refine it (`malloc`,
  `malloc(dealloc, n)`, `nonnull`, `returns_nonnull`).  The "Next" section
  below replaces the silent conversion with `unsafe`.
- [x] `#pragma tcc|GCC|clang diagnostic push / pop / ignored / warning /
  error`, `#pragma GCC system_header`, gcc's line markers in `tcc -E`.
- [x] `-fsyntax-only`; `-Wignored-attributes` (left unchecked: `access`).
- [x] Error recovery (`frontend/gen/core/recover.c`, test_max_errors.py): the
  parse goes on after an error up to `-fmax-errors=N` (8; 0 = no limit), same
  diagnostics and order at every -O.
- [x] The per-directory ratchet in yasos (`scripts/safe_mode_ratchet.py`, CI
  smoke step): warning and error counts may only go down.  It should pass
  `-fmax-errors=0`.
- [x] C23 `auto` / `__auto_type` (test_yasc_auto.py): the yasc qualifiers come
  with the inferred type; `auto q = p` with p owned is an error.
- [x] `-fdiagnostics-color`, `-fdiagnostics-format=json|sarif`
  (`driver/diag_out.c`, no columns), fix-it hints
  (`-fdiagnostics-parseable-fixits`; test_fixits.py): `move(p)`, `* view` /
  `* owned` decided from the body, `sprintf` -> `snprintf(buf, sizeof buf, ...)`.

### yasc: what the annotations mean

```c
char * owned maybe p = malloc(n);                    /* owns; may be null */
char * borrows(s) find(char *s, int c);        /* result points into s */
const char * borrows(static) err_name(int e);
struct cache { char * owned buf; char * borrows(buf) key; };
struct tokenizer borrows(text) { const char * borrows(text) pos; };
FILE * owned(fclose) maybe fopen(const char *path, const char *mode);
int owned(close) maybe fd = open(path, O_RDONLY);    /* int handle; invalid is < 0 */
void * owned maybe takes(p) realloc(void * owned maybe p, size_t n);
regex_t owned(regfree) re;                           /* an object owned as a whole */
```

- `owned`: owns its target, freed or handed on once.  `owned(dealloc)` names
  the deallocator; plain `owned` is the malloc family.
- `view`: owns nothing and does not escape (no return, no global, no
  longer-lived object).
- `borrows(x)` / `borrows(a, b)` / `borrows(static)`: points into `x`, lives
  as long as `x`, dead when `x` is freed, moved or assigned again.  Elision: a
  result written `view` borrows from all its `view` parameters; with none it
  is an error.
- `maybe`: may be null (an int handle: may be < 0).  `out`: the callee writes
  it on every path.  `drop`: the callee destroys it.  `takes(p)`: a valid
  result took over what `p` owned; a null result left `p` alone.

| where an owned value goes | needs |
|---|---|
| argument for a `view` parameter | nothing: implicit borrow |
| argument for a `drop` parameter (`free`, `fclose`) | nothing |
| argument for an `owned` parameter (not `drop`) | `move(p)` |
| `owned q = p;`, an owned field `= p` | `move(p)` |
| `return p;` (a whole local or parameter) | nothing |
| `return s->buf;` (a member, a global) | `move(s->buf)` |
| a lasting borrow in a local or a field | its type or `borrow(p)` |
| `auto q = p;` | error: `move(p)` or `borrow(p)` |

### Flow checks on the raw IR (`source/ir/lifetime.c`)

One forward dataflow over the raw-IR CFG with per-path state, then one
reporting walk in source order.  The frontend records what the IR cannot say
(pointer kinds, scope ends, call parameters: `IRLtCall` in tccir.h).  Inlined
bodies are silent; a static every call inlined gets a check-only parse.

- [x] Ownership (`-Wlifetime`): use after move or free, double free, leaks at
  return / scope end / break / continue / reassignment; `-Wimplicit-move`.
  Tests: test_lifetime.py, test_yasc_move.py.
- [x] `maybe`: dereferenced, stored in or passed to a non-maybe, returned;
  struct members by path (`q->next->v`, `s.next`, `a[1].next`, `a[i].next`),
  values across blocks, values with no name (`find(k)->v`).
  Tests: test_lifetime.py, test_lifetime_paths.py, test_lifetime_index.py.
- [x] `out` parameters, owned out-parameters with a status result
  (test_yasc_outparams.py).
- [x] Borrows: a borrow outliving its owner (up to 8 owners per pointer, past
  that "may borrow from more than 8 owners", not followed; a copy of a
  borrowing pointer borrows what it borrows),
  `view` does not escape (`tcc_ir_view_escape_check`), `borrows(member)`,
  borrowing structs, a borrow through a call's result.
  Tests: test_yasc_view.py, test_yasc_struct_borrows.py.
- [x] An owned member that is never null (`T * owned buf`) is a member path
  followed for ownership only (LT_MEMBER_NOTMAYBE, 2026-11-23,
  test_lifetime_owned_members.py): used or freed after a free, through a
  call's result; never a null report.
- [x] The method self rules (2026-11-23, test_yasc_self.py): a checked
  `struct T * view` parameter keeps the owned members of the struct valid.
  The callee may not return with one freed or moved out (null for a maybe
  one is fine).  The caller hands the struct over with them set and not
  freed: a local struct passed by `&s` stays followed (it was "not
  followed" before), and its members still own memory after the call (maybe
  ones may be null).  `drop struct T *self` frees them: the callee
  must free every one (its struct's owned members are paths from the
  start), and the caller's count as freed (`defer s.clear()`).  An inlined
  method's self binding is the same handing over, so -O changes nothing.
  Not covered: members of members (`b->in.buf`, `b->next->buf`), owned
  variants of tagged unions (the tag check's), `struct T * owned self`.
- [x] `owned(dealloc)` (test_yasc_dealloc.py; elements of local structs and
  arrays too, test_lifetime_loop_bounds.py), int handles
  (test_yasc_handles.py), `takes(p)` (test_yasc_takes.py), owned items of
  local structs and arrays (`tcc_ir_slot_check`, test_yasc_slots.py), owned
  array elements with loop summaries (test_lifetime_index.py), objects owned
  as a whole (test_yasc_objects.py).
- [x] Correlated conditions (`if (c) free(p); ... if (!c) ...`): predicates
  and up to 8 contexts per block (test_lifetime_paths.py).
- [x] `-Wuninitialized` / `-Wmaybe-uninitialized` (all C, safe default),
  `-Wreturn-local-addr` (-Wall), `-Wdangling-pointer` (safe default).
- [x] `-Wnull-dereference`: after optimisation, level dependent by design.
- [x] Resource pairs through libc annotations (`sys/_attrs.h`, `sys/yasc/`):
  malloc family, fopen/fclose, popen, opendir, dlopen, file descriptors,
  getline, regcomp, pipe.  Not annotated on purpose: `freopen`, `realpath` /
  `getcwd` / `tmpnam`, `mmap` (fails with `MAP_FAILED`).
- [x] `-Wtainted-format` (`source/ir/taint.c`, off by default): untrusted
  input reaching a format, through globals, members, helpers, calls through
  pointers and across units (`.tcc.taint` records, checked at the link).

**What the flow checks do not follow** (the input for S1 below):

- a variable whose address is taken, unless every use goes to an owned
  out-parameter or a parameter that only reads (an owned one is said: "the
  address of 'p' goes where it may be written: the lifetime check does not
  follow 'p'"); a local struct or array whose address goes to a pointer that
  may write, other than a checked `view` (closed 2026-11-23 for `view`: the
  method self rules below);
- a store through a pointer whose base is not known (`f()->next = 0`)
  forgets no member; a store into `a[i]` does not make `a[j]` "may own"
  (closed 2026-11-23: a store to `r->next` forgets `q->next`, a call or
  inlined body forgets the globals' members, a release of `a[i]` makes `a[j]`
  "may be released");
- globals are not followed inside an inlined body;
- a call result used as a base (`f()->next`; an owned member there is said); an index that is not a constant or an int variable plus a constant
  (`a[i * 2]`, `a[f()]`) and `&a[i]` handed on (said: "'a' is reached in a way
  the lifetime check does not follow"); maybe members at `a[i + 1].next`;
- borrowing structs over 256 bytes, in arrays, or from `?:` of two calls;
- status codes instead of `takes` (`int push(..., void * owned item)`);
- caps: 32 predicates, 8 contexts per block, 16384 contexts and 1 MB of states
  per function, 64 array items, 16 index items, 16 loop summaries -- past
  them the result is less precise (and may differ at -O1+ in a big function);
- loops other than `for (i = C0; i < N; i++)` left only by their test give
  nothing definite about owned elements.  With a variable bound
  (`for (k = C0; k < n; k++)`, n a parameter or local the loop does not
  write) the elements C0..n-1 are a range of their own, released by a later
  loop over the same range -- any index, the same bound or a copy (`m = n`,
  neither written again), the same start and test; another bound or start,
  or a bound written anywhere, leaves the range owned ("may leak"); counting
  down with a variable bound is not followed (test_lifetime_loop_bounds.py);

### Frontend checks

Same result at every -O, once per place.

- [x] `-Wformat`, `-Wformat-security` (safe default), `-Wformat-n` (error in
  the safe default); strftime, strfmon, `format_arg`, `%N$`, conditional
  formats (test_format.py).
- [x] `-Wunused-result` / `[[nodiscard]]`, `-Wnonnull`,
  `-Wdeprecated-declarations`.
- [x] `-Wunused-*` (variable, function, but-set, value, label,
  local-typedefs; parameter in -Wextra) (test_unused.py).
- [x] `-Wimplicit-fallthrough`, `-Wempty-body` (safe default),
  `-Wparentheses`, `-Wdangling-else`, `-Wlogical-not-parentheses`, `-Wswitch`
  (-Wall), `-Wshadow` (only when asked).
- [x] `-Wsign-compare`, `-Wconversion`, `-Wsign-conversion` (not in -Wall).
- [x] Constant expressions: shift-count, shift-overflow, div-by-zero,
  overflow, tautological-compare (-Wall), type-limits (-Wextra).
- [x] `-Warray-bounds` (constant subscripts, the `for` loop pattern; `=2`:
  intervals on the raw IR, `ir/bounds.c`), `-Wsizeof-pointer-memaccess`,
  `-Wsizeof-array-argument`.
- [x] `-Wstrict-prototypes` (safe default; `()` is `(void)` in C23 and yasc),
  `-Wmissing-prototypes`, `-Wpointer-arith`, `-Wcast-qual`, `-Wcast-align`
  (all only when asked), `-Waddress-of-packed-member` (-Wall).
- [x] `-Wextra` as a group.
- [x] `-Wcross-tu` (off by default): a symbol's type or a struct's layout
  differing between units, compared at the link (`.tcc.typesig`).

### Stack (process stacks are 4 to 8 KB)

- [x] `-Wframe-larger-than=N`, `-Wstack-usage=N`, `-fstack-usage` (`.su`),
  `-Wvla` / `-Walloca` (safe default).  Test: test_stack_usage.py.
- [x] `-fcallgraph-info[=su]` (`.ci`) and `scripts/stack_usage.py`: the worst
  path per entry point, indirect calls resolved by calling shape, `.ci` for
  libtcc1 and libc (host-side, never in a target image).

### Object sizes

- [x] `__builtin_object_size` / `__builtin_dynamic_object_size`
  (`builtin/objsize.c`), decided by the parser: the same at every -O.
- [x] `_FORTIFY_SOURCE` with yasos libc (`sys/_fortify.h`, test_fortify.py).
- [x] `-Wstringop-overflow`, `-Wformat-overflow`, `-Wformat-truncation`,
  `-Walloc-size-larger-than=N`, `-Wfree-nonheap-object`,
  `-Wmismatched-dealloc` (-Wall); `-Walloc-mul-overflow` (when asked).

### Runtime instrumentation

- [x] `-fsanitize=undefined` (`op/ubsan.c`): trap mode by default
  (`udf #254`), handler mode with clang's `__ubsan_handle_*` ABI; `-ftrapv`.
  Test: test_sanitize.py.
- [x] `-fstack-protector[-strong|-all|-explicit]`, `stack_protect` /
  `no_stack_protector` (test_stack_protector.py).
- [x] `-b` bounds checking, the armv8m cross and QEMU only
  (`op/bcheck.c`, `lib/bcheck-bm.c`, test_bcheck.py).
- [x] `-ftest-coverage` (tcov).

## Open items

- [ ] Error recovery, -O dependent: a stray `{` or `}` in a body saved as
  tokens (-O1+) moves its end; a body deferred to the end of the unit sees
  names declared after it; after an error `out = 1;` in yasc reads differently
  in a deferred body.
- [ ] A void `static inline` whose every call is dropped without a parse says
  nothing at -O1+ under `-ccompat` without -Wall.
- [ ] `deprecated` on `tmpnam`, `sprintf`, `strcpy`, `strtok`, `atoi` in libc
  `sys/yasc/`: not there yet.  Once it is, the ratchet counts every call.
- [ ] `-Wmissing-prototypes` in the safe default: 815 reports over our C, 615
  of them in yasvi's test files; would raise the ratchet floor.
- [ ] `-Wconversion` against gcc: `-Wfloat-conversion` constants and a
  builtin's result type differ.  `-Wtautological-compare`: no commuted
  operands, and reported inside a macro expansion (gcc is quiet).
- [ ] YasOS stack margins from the tool rather than by hand: build the
  kernel-tcc C and the apps with `-fstack-usage -fcallgraph-info`, run
  `stack_usage.py -e <functions taking the lock>` for each `RankedMutex` with
  `stack_needed` (`fs_lock.zig`, `ext4fs.zig`, the page pool), keep it true
  with a CI step using `--limit`.  Assembly members (`__aeabi_ldivmod`,
  memcpy, setjmp, vfork) and the fp libraries need `--budget`.
- [ ] `-b`: one member overflowing into the next, structs passed or returned
  by value, library functions through a pointer or not wrapped (stpcpy,
  mempcpy, memchr, strnlen, printf family, siglongjmp), a scalar's `&x + 1`,
  regions first registered after a return a loop reaches again.
- [ ] `-Wtainted-format`: memory through pointers of unknown origin, a large
  struct copy made by a helper call, a summary's write through a parameter
  past the seventh.
- [ ] `mmap` ownership: a `maybe` with another sentinel than null.

## Next: strict lifetimes, `unsafe`, checked `restrict`, and what the optimizer may then assume (planned 2026-11-23)

Decided 2026-11-23: in yasc a lifetime violation is an error, a regular-C
pointer becomes an annotated one only inside `unsafe`, `restrict` is the
exclusive borrow and is checked.  Target: new yasc projects (the debugger,
the bootloader); `-ccompat` code is not touched and gains nothing.  Nothing
below is implemented.

The rule that makes the rest sound: **an optimizer fact comes from a declared
type, never from the flow analysis, and only from a type that had to be
checked** -- a yasc declaration, in a file compiled without `-ccompat`, with
no error reported.  `unchecked` pointers carry no fact at all.

### S1: violations are errors

- [x] Done 2026-11-23 (test_yasc_unsafe.py): in the safe default
  `-Wlifetime`, `-Wimplicit-move`, `-Wdangling-pointer` and
  `-Wreturn-local-addr` are errors (`WD_STRICT`, `TCCState.strict_lifetime`),
  and so are the yasc ownership type rules (`tcc_yasc_rule`: said under
  -Wlifetime there, always-on warnings under -ccompat).  `-Wno-X`,
  `-Wno-error=X` and `#pragma diagnostic` cannot lower them ("... is an error
  in yasc and cannot be lowered: use unsafe { } or -ccompat").  -ccompat
  keeps them warnings.  `-w` hides warnings but not these (diag_is_strict;
  the flow checks run under -w in the safe default).
- [x] Done 2026-11-25 (test_yasc_init.py): yasc enforces initialization.
  `-W[maybe-]uninitialized` reads are errors in yasc (under -Wlifetime), the
  "may" case too -- the check runs on the raw IR, so the answer does not
  depend on -O.  A scalar's address may be taken only where it has a value
  (an `out` parameter's argument sets it); what the check cannot follow
  (arrays, structs, unions, complex) needs an initializer or `= undefined`.
- [ ] Stays a warning: the post-optimisation `-Wnull-dereference` (its result
  depends on the -O level; an error must not).
- [x] Inventory closed 2026-11-23: every case is reported, or the check says
  it does not follow the value (those "does not follow" warnings become the
  errors S2's `unsafe` gets past).
- [ ] **What the checker cannot follow, it rejects.**  Each entry of "What the
  flow checks do not follow" above is either closed or turned into an error
  that asks for `unsafe`.  The inventory is
  `tests/ir_tests/test_yasc_strict_inventory.py`: one strict-xfail bug per
  entry (the caps excepted); a case that starts passing fails the run until
  its marker goes.
- [ ] yasos: `scripts/safe_mode_ratchet.py` counts the new errors per directory;
  the examples (`examples/yasc`) and libc `tests/annotations` are fixed in the
  same change.

### S2: `unsafe`

- [x] Keyword, reserved in yasc only (a name `unsafe` in scope wins).  Two
  forms: the block `unsafe { ... }` and the expression `unsafe(expr)`
  (2026-11-23, test_yasc_unsafe.py).  They turn the strict options off for
  the region's lines: the live options for what the parse says, and records
  in the #pragma diagnostic line table for the flow checks (`unsafe` edges
  carry the file's options at those lines; the lookup takes the closest
  record at or before a line, since bodies parsed at the end of the unit at
  -O1+ add their records late).  By lines, so a region has its lines to
  itself (2026-11-23): `unsafe {` begins its line (the token before it is on
  an earlier line: tccpp.c tok_prev_line), its `}` ends it, and a statement
  holding `unsafe(...)` is alone on its line -- else an error.  An error recovered from inside a region ends it
  where the parse goes on.  The region ends at the line of the first token
  after it (the line after its first one when that token is on it).
- [ ] Errors outside `unsafe`:
  - [x] an `unchecked` pointer stored in, passed as or returned as `owned` /
    `view` / `borrows(...)`: "a pointer from regular C becomes a view here:
    write unsafe(...)" (`-Wunchecked`, strict; 2026-11-23,
    test_yasc_unsafe.py); null and `(T *)0` need nothing; `restrict` when S3
    adds it;
  - [x] a dereference, index or member access through an `unchecked`
    pointer (2026-11-23, indir.c + unary_subscript -> yasc_unchecked_deref,
    -Wunchecked); a regular-C macro's own dereference (`errno`) is regular C,
    a call through a C function pointer is not memory;
  - [x] explicit casts to an owned / view / borrows pointer (2026-11-23,
    assign_check.c yasc_cast_check, -Wunchecked): from regular C, from an
    integer (MMIO, flash offsets; 0 is fine), dropping maybe, making a
    pointer that owns nothing owned, an owned one a view (write borrow),
    dropping const; `restrict` with S3;
  - [x] `va_arg` of a pointer: va_arg's type is written in a regular-C
    macro, so its result is unchecked and -Wunchecked covers it;
  - [x] unions (2026-11-23, assign_check.c yasc_union_member_end,
    test_yasc_unions.py).  A union member may have been written as another
    member, so the checks cannot know what it holds:
    - U1: a union without owned / view / borrows pointers (or handles) is
      not checked at all -- registers as `raw` + bit-fields, float bits,
      protocol buffers.  Punning scalars is defined (C99 TC3, gcc).
    - U2: a pointer member reached through a union (`u.s`, `x.u.st.p`,
      `u.arr[i]`; the chain is `SValue.union_member`, kept for nested
      members in yasc) reads as regular C, `unchecked`: held, compared and
      passed to C freely, owned / view / memory only in `unsafe`
      (-Wunchecked).  A store to it (`u.s = p`) is checked as its type
      says (a view parameter stored there still escapes).  Its address
      (`&u.s`), or an aggregate member holding such pointers used whole
      (copied, passed, returned, addressed), needs `unsafe`.  An owned
      member in a union (also in a struct or array member of it) is an
      error under -Wlifetime: which member is live is not known, so it
      could be neither followed nor freed (Rust needs `ManuallyDrop` there).
    - [x] U3 tagged unions (2026-11-23, decl/struct.c, lifetime.c
      tcc_ir_tag_check, test_yasc_tagged_unions.py), like Zig's
      `union(enum)` but with the tag an explicit member, checked at compile
      time: `union(k) { long i when(V_INT); char * view s when(V_STR); } u;`
      as a named member of the struct whose earlier member k (integer or
      enum, not a bit-field) is the tag.  Every member says when(K, ...);
      at most 31 values.  A use of a member (read, write, address) must
      have k known to select it on every path: the state per tag (base,
      offset) is the mask of values it may hold, narrowed by the edges of
      `CMP tag, #c` / `TEST_ZERO tag` (if, &&, ?:, early return, switch
      with its range compares), set by a constant stored in the tag,
      forgotten by any other store to it (or to the same member through
      another base), by assigning the base, by a call that may write
      through the base (not `const T * view`) and, for a global, by any
      call.  Bases: a pointer variable or parameter, a local struct, a
      global struct.  A tagged member is not a U2 read (a view in it stays
      a view); the chain below it (`v->u.st.path`, `v->u.arr[i]`) neither
      (unary.c yasc_tchain: the union_member mark cannot tell tagged from
      untagged).  -Wlifetime, strict; unsafe lowers it.
      Also (2026-11-23, second step):
      - a switch on the tag (`switch (v->k)`) with no default handles
        every value the members name (block.c check_switch_tagged, a yasc
        rule at the switch's line), as Zig's switch on a union(enum);
      - the tag copied to a local (`enum vkind k = v->k; switch (k)`): the
        local mirrors the tag (state entries after the masks, kept by a
        join only where every path agrees) until the local or the tag
        changes or a call may write the tag; temps copied from it (the
        switch's dispatch copy) too;
      - a tag through a global pointer (`gp->k`): base kind 4, forgotten by
        a store to the pointer or any call;
      - owned members (pointers or handles, also inside a variant's struct)
        are allowed: per owned item a "may still own" bit, set by a store
        to it, cleared by passing its value to an owned/drop parameter
        (free, take(move(...))), by moving it into a local, or when the
        tag cannot be its variant.  The variant changing (a store to the
        tag) or the item written over while it may own is said; a local
        struct's leak is the slot check's.  Double free / use after free
        through the item are the member paths'.  Through a pointer or a
        global, an item released here (freed, moved out) while the tag
        still selects it at a way out of the function is said: the caller
        would free it again (the counterpart of the method self rule for
        ordinary owned members); a new value, null, or a variant change
        ends it.
      - member paths of nested members (`v->in.p`) are recorded with their
        offset from the outermost object (unary.c lt_chain): they were
        named `v[1].p`, element 1 of an array of `in`s.
      - a switch on a local the tag was stored into (`enum vkind k =
        v->k;`, by its VAR vreg, per function) is exhaustive too.
      Globals past a call (2026-11-23, lifetime.c tcc_ir_call_writes_global,
      IRLtCall.gkeep, test_yasc_global_calls.py): a direct call to a
      prototype a system header gives, with no function pointer parameter,
      writes only the globals system headers declare (TCCState.sys_globals:
      errno, stdout); a leaf callee (__attribute__((leaf)), now parsed)
      cannot reach this unit's statics; other calls, a call through a
      pointer, qsort / atexit and the like (callbacks) forget every global,
      as does the start of an inlined body.  So `free(g.p); free(g.p);`
      is said.  The tag check follows the same rule.
    - Handle members (`int owned(close) maybe fd;`) parse since 2026-11-23
      and a union refuses them like owned pointers; a handle read through a
      union drops to a plain int.
- [x] int handles as struct members (2026-11-23, decl/method.c: `owned(`
  is a qualifier in a struct body, never a method name;
  test_yasc_handle_members.py).  `int owned(close) maybe fd` members are
  member paths of kind LT_MEMBER_HANDLE (IRLtMember.owned is LT_MEMBER_*
  flags): a local struct's handle leaks, closed twice / used after close
  through a pointer is said, an untested one to a non-maybe parameter is
  said, and `c->fd >= 0` / `< 0` / `== -1` tested in place refine the path
  (lt_handle covers paths).  Not followed, as for pointer members: globals
  (`g.fd` closed twice), non-maybe handle members.
- [x] `-Wvolatile-bitfield` (2026-11-23, vstore.c, strict, yasc only): a
  bit-field store to volatile memory (`REG->b.en = 1`, `|=`, `++`) is a
  read-modify-write of the whole unit (tcc accesses it at the container's
  width, as AAPCS asks): a W1C flag read as 1 is written back and cleared,
  a write-only register is read, an interrupt or the other core between
  the load and the store is lost, and RP2350's set/clear aliases are
  bypassed.  The error asks for the explicit form -- copy to a local,
  change it there, store it back; reads are free; `unsafe` lowers it.
- [x] Register fields (2026-11-23, decl/struct.c, expr/unary.c, store/vstore.c,
  ir/regaccess.c, test_yasc_register_fields.py; -Wregister-access, strict,
  yasc only): `ro`, `wo`, `w1c` after a member's declarator (bit-field or
  whole; plain names elsewhere).  An `ro` field of volatile memory written
  or a `wo` one read (`=`, compound, ++/--; not its address) is said in the
  frontend.  A store to volatile memory whose type -- or the object the
  member stored is part of, at its place (`r->raw` of a union whose
  `b.irq` is w1c) -- has w1c bits is noted; ir/regaccess.c follows, on the
  raw IR, which bits of each value still hold what a volatile read gave
  (AND / OR with a constant make bits known, other operations on such a
  value give all; frame words and locals carry it, a join is the union) and
  says a noted store that writes a w1c bit back as read.  Registers up to
  64 bytes: a mask per word, the store's word found from its offset (word
  copies) or the local's words checked when the copy is a memmove; a
  register struct copied into a local through memmove makes the local
  hold what was read.  A call handed a pointer to volatile memory (put.c
  notes it) may read a register: its result, and what it writes through
  a local's address, hold what was read.  Not followed: a pointer to the
  register cast to a non-volatile one first (`memcpy(&v, (void *)r, 4)`),
  registers past 64 bytes.
- [ ] Free outside `unsafe`: holding, copying, comparing and passing an
  `unchecked` pointer to regular-C code; a `view` argument to an unchecked
  parameter (the optimizer then assumes it escapes and is written).
- [x] An `owned` variable passed to an unchecked parameter says
  `borrow(p)` or `move(p)` (-Wimplicit-move, a fix-it adds borrow); `move(p)`
  is followed as a move into the call (2026-11-23, test_yasc_move.py; the
  tutorial's chapter 6 teaches it).
- [ ] `sys/yasc/*.h` stays the trusted layer: declarations believed, bodies
  not checked.  libc `tests/annotations` is the only guard; say so in the
  header README.
- [ ] `unsafe` is not carried into the IR: what leaves the block is an
  annotated pointer, trusted like any other.
- [x] `-funsafe-report` (2026-11-23): "file:line: note: unsafe region",
  once per region however often its body is parsed, for review and for the
  ratchet to count.
- [ ] Not in the first version: `unsafe` functions (callers must be in
  `unsafe`).  Add when the debugger shows the need.

### S3: `restrict` is the exclusive borrow

Done 2026-11-23 (test_yasc_restrict.py), first version:
- [x] Kept in the type: `T * restrict view p`, `T * restrict borrows(x) p`
  (`YASC_RESTRICT` 0x100 in the int qualifiers; the pointer node keeps it in
  its `ymod`, part of the interned key).  Regular C drops it as before.
- [x] What becomes a restrict pointer (argument, initializer, assignment,
  return) must be exclusive: an owned pointer, another restrict pointer, a
  local's address or a local array.  A named view is "a view may be shared:
  a restrict pointer needs ..."; a global's or static's address is "... the
  code can reach it by name too: write unsafe(...)" (assign_check.c
  yasc_verify_restrict, -Wlifetime).
- [x] At a call, no other argument may point into the object a restrict
  argument points into: the same variable (a pointer that owns nothing as
  its owner), the same local frame object, through copies and offsets in
  the block (lifetime.c lt_arg_source / lt_restrict_call, reporting walk):
  "argument 1 of 'cpy' is restrict, but argument 2 points into 'p' too".
- [x] restrict to plain view converts freely; regular-C to restrict is
  -Wunchecked's.
- [x] A restrict local's borrow lasts while it is still in use: what it was
  made from (an owned variable, a local object) reached directly before r's
  last use is reported (lifetime.c tcc_ir_restrict_check: r's sources must
  agree, r's own liveness over the CFG; 2026-11-23).  Not followed: a
  pointer derived from r and kept past r's last use, r assigned from
  different sources.
- [x] `p + n` keeps what p is to the checks (type_yasc_derive): a view stays
  a view (so `v + 1` cannot become restrict), restrict stays restrict, a
  regular-C pointer stays unchecked (it got past -Wunchecked before);
  ownership goes.
- [x] Local arrays and structs are named in the messages (ir->lt_fnames,
  2026-11-23).
- [ ] libc: `sys/yasc/string.h` and friends repeat the `restrict` of the C
  prototypes (memcpy, strcpy, snprintf, ...) -- breaks callers passing views
  (they need an exclusive pointer or unsafe); with the libc branch.

### S4: optimizations

All under `-fyasc-facts` (off at -O0; opt-in at first, default at -O1+ once
the debugger and bootloader build and pass with it), one sub-flag per group so
a suspected miscompile is ruled in or out by switching one off.  Each group
lands with a counter (`-dump-ir-passes` statistics) of how often it fired on
`examples/yasc` and the first yasc project.

- [x] **O0 plumbing** (2026-11-23): `IRYfCall` by call id (tccir.h
  `ir->yf_calls`, freed with the function's IR), filled at the call by
  `tcc_ir_yf_note_call` from the callee's declared types when
  `-fyasc-facts`, the safe default (strict lifetimes) and -O1+: `noescape`
  (a `view` parameter, not owned / unchecked, not named by the result's
  borrows(...) nor a borrowing struct result) and `readonly` (`const T *
  view`).  `tcc_ir_yf_param` answers by IR parameter index (the struct-return
  buffer counted).  `TCC_YF_STATS=1` prints what each pass used.  Still to
  add: exclusive mask (O4), result fresh / non-null (O2, O7), the function's
  own parameters.  The facts are the declared contract: a callee compiled
  under -ccompat is not held to it (doc'd; sys/yasc headers are trusted).
- [ ] **O1 `view` does not escape.**  A local whose address only reaches
  `view` parameters is written by that call (unless read-only) and by nothing
  after it.  There is no shared "may this call clobber this slot" query: each
  pass decides escape itself, so each consumer is wired on its own.
  - [x] sl_forward (2026-11-23): a frame object whose address (and the temps
    copied or offset from it) goes only to memory accesses, comparisons and
    no-escape parameters is not addrtaken; a call it is handed to through a
    parameter that may write invalidates it (`yf_classify`).  Anonymous
    slots (structs, arrays) only: named scalars with `&x` are addrtaken VARs,
    another path.
  - [x] copy_fwd (2026-11-23): such an argument is a read (and an inexact
    write unless read-only) at the call, not an escape; the call clobbers the
    copy only through it.
  - [x] named scalar VARs (2026-11-23, `yf_promote`, after ptr_local_fwd,
    once per function): when every `T <-- &x` feeds only no-escape
    parameters, x gets a register VAR; its slot is written just before each
    such call (only where x may hold a value: a forward pass over the CFG,
    so an out parameter gets no store of garbage) and read back just after a
    call whose parameter may write.  Scalars only (32/64-bit, float), up to
    32 per function; not with inline asm or nested functions.
  - [x] dead_local_slot (2026-11-23): an address handed to a no-escape
    parameter keeps its slot tame, and the call reads the whole object at
    its position -- a store after the last such call is dead (`t.a = 99`
    after `use(&t)`).  A store through STORE_INDEXED is not: that path is
    gated on there being no indexed instruction at all, and narrowing the
    gate miscompiled plain C (bug_orphan_jump_hop, bug_bitfield_packed10).
  - [x] entry_store_prop (2026-11-23): an object whose address goes only
    to read-only no-escape parameters (`const T * view`, directly or
    through a single-def temp) keeps its entry-block stores: a constant
    field read in a loop after the call is the constant.  Phase 3c also
    forwards into a read folded into arithmetic (`T2 ADD StackLoc[-20]`,
    any code: -0.014% at -O2 and -Os, nothing bigger).
  - not done: dse (flow-insensitive: "read by the call" is "read") and sra
    (the object is needed in memory at each call anyway; what is left is
    forwarding, which sl_forward, dead_local_slot and entry_store_prop do).
  Run test: test_yasc_facts.py (QEMU, with and without the facts, -O0 as
  the reference; breaking the write invalidation, the scalar reloads or the
  scalar stores makes it fail).
- [ ] **O2 fresh `owned` result.**  A call's owned result aliases nothing
  loaded or received before the call.
  - [x] load_cse (2026-11-23): a FUNCCALLVAL whose callee's result is
    `owned` (not unchecked, not takes(p)) is a root like an O4 parameter
    (IRYfCall.fresh); handing it to a `drop` parameter (free) keeps it in
    (IRYfCall.drop), as does a no-escape parameter.  Run test: case_fresh.
- [ ] **O3 valid for the pointee.**  A non-`maybe` yasc parameter can be
  loaded from for `sizeof(*p)` bytes at any point of the function where it is
  still live (not moved, not dropped): invariant load hoisting out of
  zero-trip loops, if-conversion over a load.  Only the pointee at offset 0;
  yasc checks no bounds, so `p[i]` gets nothing.
- [ ] **O4 exclusive.**  After S3: memory reached through a restrict or owned
  parameter is not touched through any pointer not derived from it --
  loads survive other stores, loop bounds stay in registers, memmove becomes
  memcpy.
  - [x] load_cse (2026-11-23; a no-escape or drop argument keeps it in): the function's `T * restrict view` and
    `T * owned` parameters (ir->yf_excl, recorded with the variables); per
    parameter P, flow-insensitive over the flat IR, m = may derive from P,
    d = derives from P (every def); the fact holds only if every use of a
    value in m is a memory access, a comparison, a no-escape parameter or
    another derivation.  A tracked load through a base in d survives a store
    through a base not in m (and the other way round): stores through a
    temp, a parameter or a variable.  Run test: test_yasc_facts.py (a field
    and an element reloaded, a store through P, a constant and a run-time
    offset from P -- an always-apart oracle makes it fail).
  - [x] loads folded into arithmetic operands (2026-11-23): load_cse's
    exposure step (`ssa_opt_expose_cmp_loads`) now splits a pointer load read
    at least twice out of ADD/SUB/MUL/AND/OR/XOR as well as CMP, so the facts
    see it -- not when the result is stored back to the same address within
    a few instructions (read-modify-write keeps its folded form).  Without
    the facts too: tests/ir_tests -O2 -0.05%, -Os -0.08% (26 / 25 files
    smaller, 5 / 4 bigger, the largest +224 bytes).
  - [x] LICM (2026-11-23, the field-pointer hoist `ssa:licm_field_ptr`): a
    load through a base derived from P stays invariant over a store through
    a base that cannot derive from P, and over a call not handed such a
    pointer (or handed it as a read-only view); the zero-trip handling is
    the pass's own.  The alias facts (YfAlias) moved to ir/lifetime.c
    (tcc_ir_yf_alias_*), shared with load_cse; they now take a variable
    read in place (lval STACKOFF) for a value -- they took it for memory,
    so a store through `e = d + k` looked apart from `d[1]` (caught by
    case_licm_derived).  Run test: case_licm_field / self / derived /
    handed; calls taken for not writing, or every store for apart, fail it.
    Not done: a second read after a call in the same trip stays.
  - [ ] sl_forward's pointer stores, memmove -> memcpy.
- [ ] **O5 an owned member's pointee is not its struct.**  A store through
  `b->data` does not change `*b`.  Needs the rule "an object does not own the
  storage it lives in" in the checker first.
- [ ] **O6 stores before a drop are dead.**  `memset_explicit` and volatile
  exempt.
- [ ] **O7 non-null.**  Fold `p == 0` on a non-`maybe` pointer.  Last, and
  not default: a guard written on purpose (a bootloader checking corrupted
  input) needs a spelling that keeps it -- a test of `(T * maybe)p` is kept.
- Not planned: heap to stack for a non-escaping `owned` local (4-8 KB process
  stacks); call-level purity (does not follow from pointer annotations).
- `volatile` pointees and everything `unchecked` get none of the above.

### Order

1. S1 inventory of the "not followed" cases, then S1 errors + ratchet.
2. S2 `unsafe` (the S1 rejections need it to be usable).
3. O0, O1, O2 -- they need no new checker rule.
4. S3 `restrict`, then O4.
5. O3, O5, O6; O7 behind its own flag.

Validation per step: ir_tests with a must-fire and a must-not-fire case per
fact (the unsafe / unchecked / volatile variants of the same source), the
debugger built with each group on and off and its tests run on QEMU; the
self-host and the fuzz once, on the final tip.

## Later

- [ ] **Thread-safety annotations** (clang `-Wthread-safety`: `guarded_by`,
  `requires`, `acquire`/`release`).  Deferred (2026-11-20): YasOS has no
  threads yet.  Design: attributes `capability("mutex")` / `lockable` on a
  type, `guarded_by(m)` / `pt_guarded_by(m)` on a global or member,
  `requires_capability(m)`, `acquire_capability(m)` / `release_capability(m)`
  on functions, `no_thread_safety_analysis`; kept in SymLocalFacts (SymAttr is
  full; new 1-bit facts are `SYMF_*` flags there), the lock named by its
  token.  A frontend hook per call records which lock it takes or releases;
  `source/ir/threadsafety.c` runs on the raw IR like ir/taint.c: a bit per
  lock "held", a join ANDs.  Limits: locks by name only, no try-lock edges at
  first, guarded members through one base only.  Off by default.
