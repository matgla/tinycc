# Static analysis: what tcc is missing

TODO list (2026-11-20) for the checks a safe-by-default tcc should have.
The comparison points are gcc (`-Wall -Wextra`, `-fanalyzer`), clang (`-Wall`,
`--analyze`) and Cake's flow analysis. libc's half (header annotations, runtime
hardening) is in `libs/libc/docs/static_analysis_todo.md`.

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
  (`source/ir/lifetime.c`). The regalloc SSA is built after the frontend
  inliner (-O1+) and the flat passes, so it cannot promise that. A check that
  needs proven values runs after optimisation and may differ by level:
  `-Wnull-dereference` (`tcc_ir_null_deref_check`, after the regalloc
  pipeline). Flow `-Warray-bounds` is a loop pattern the parser reads
  (level 1) plus, at `-Warray-bounds=2`, an interval analysis on the raw IR
  (`source/ir/bounds.c`, no SSA needed): both the same at every -O.

## Already there

- [x] `-Wall` set: implicit-function-declaration, discarded-qualifiers,
  int-conversion, incompatible-pointer-types, return-type, implicit-int.
  Outside it: write-strings, unsupported.
- [x] Safe by default: the six above are errors, and `-ccompat` turns all of it off.
- [x] yasc type rules (step A): `owned` / `maybe` / `view` / `out` / `drop`,
  owned and non-owned pointers kept apart, owned call results discarded or
  never freed. Plain pointers are still accepted; P0 below forbids them.
- [x] `-Wlifetime` flow checks (step B, see P1): ownership, `maybe`, `out`.
- [x] `-Wuninitialized` / `-Wmaybe-uninitialized`, `-Wreturn-local-addr`.
- [x] `-fdiagnostics-color[=always|never|auto]`, and `tcc_note()` for
  "note:" lines that follow their warning (dropped with it).
- [x] `-ftest-coverage` (tcov). C23 `#embed` / `__has_embed`.

## P0: needed to move our own code off `-ccompat`

- [x] **Plain pointers are forbidden in yasc** (decided 2026-11-20). Every `*`
  says `owned` or `view`; `maybe`, `out` and `drop` combine with either. A bare
  `T *` is an error: "plain pointer in yasc: say owned or view".
  - [x] `view` qualifies a pointer, after the `*` like `owned`: each level says
    what it is (`char * view * owned argv_copy`). After a `*` the qualifiers
    are never names, so abstract declarators take them (`f(char * view)`).
  - [x] Everywhere a pointer type is written: parameters, returns, members,
    globals, locals, typedefs, casts, compound literals, `sizeof`. A typedef
    that already says it satisfies it. An array decaying, a string literal and
    `&x` are expressions, not written pointers.
  - [x] Function pointers are exempt (they can only borrow), checked once a
    nested declarator's suffix is applied: `int (**fpp)(int)` is still an error.
  - [x] **The dialect is per file, not per unit.** yasc applies to the main
    file under `-std=yasc` (and the safe default) and to any file from its
    `#pragma yasc` on. Every other header is regular C: the yasc words are
    identifiers there, and so in the expansion of a macro defined in regular C
    (markers around it, kept in saved bodies) and in the replay of a function
    defined in regular C. `__YASC__` is defined for the unit.
  - [x] A pointer declared in regular C is **unchecked** from yasc code
    (`YASC_UNCHECKED`, docs/yasc_modules.md "Legacy C"; replaced the earlier
    "reads as `view maybe`"): it converts to and from owned and view pointers
    silently and -Wlifetime takes it as neither maybe nor null. GCC attributes
    refine it: a `malloc` result is `owned maybe`, `malloc(dealloc, n)` makes
    the deallocator's argument owned, `nonnull` is checked (-Wnonnull). yasos
    libc carries them on its allocators (libc branch feat/yasc-attributes).
  - [x] A yasc include path, `-iyasc DIR` (2026-11-20): searched as `-I`, in
    its place among them; a header found there, or a `"..."` header next to
    one, is a yasc file as if it began with `#pragma yasc`.
    `YASC_INCLUDE_PATH` adds them from the environment (after the command
    line's, as CPATH); `tcc -E` writes the `#pragma yasc` such a header stands
    for (test_yasc_include_path.py).
  - [x] A regular-C macro's expansion in yasc code is regular C, and so are
    the arguments written inside it; a yasc macro used inside a regular-C one
    reads as regular C too. The markers did all that already; what was left
    (2026-11-20, test_yasc_c_macros.py): a `*` is in the dialect it was
    written in -- `PTR(char) p` with a regular-C `PTR` was a "plain pointer"
    error because `p` after the expansion is yasc again (now unchecked;
    `PTR(char) view p` qualifies it) -- and tcc's `va_arg` was an
    object-like alias of `__builtin_va_arg`, so its type was read as yasc;
    it is function-like now, as gcc's (`va_arg(ap, char *)`). Not done: a
    regular-C macro's expansion is not marked in `tcc -E` output.
  - `-ccompat` is unaffected. Under it the keywords are ordinary identifiers.

- [x] **`#pragma tcc diagnostic push / pop / ignored / warning / error "-W…"`**,
  also spelled `GCC` and `clang`, and through `_Pragma`. `warning` beats
  `-Werror`; `"-Wall"` switches the -Wall set. Each change is recorded with
  its file and line, so a diagnostic reported later than its line was parsed
  (the flow checks, at the end of a function) sees the options at its line.
  An unknown option is ignored (reported under `-Wunsupported`); `tcc -E`
  keeps the lines; each unit starts from the command line's options.
- [x] **`-fsyntax-only`**: every function gets the check-only parse (frontend
  diagnostics and the flow checks, no code), each file as for `-c`, nothing
  written, no link.
- [x] **Warn on attributes that are ignored but carry checks**
  (`-Wignored-attributes`, the safe default; not in -Wall, which would stop
  -Wall -Werror builds of code using `format()`, tinycc's own among them): `format`, `nonnull`, `returns_nonnull`,
  `warn_unused_result`, `nodiscard`, `malloc`, `alloc_size`, `access`,
  `deprecated`, in either syntax, once per attribute in a unit, not for system
  headers (`BufferedFile.sys`). `fallthrough` and `maybe_unused` stay silent:
  they only silence warnings tcc does not give. Each P1 item below that
  implements one of them takes it off the list (`unchecked_attributes[]`):
  only `alloc_size` and `access` are left. `malloc`, `malloc(dealloc, n)`,
  `nonnull`, `returns_nonnull`, `format`, `warn_unused_result` and
  `deprecated` apply in both spellings, `__attribute__((...))` and
  `[[gnu::x]]` (decl/attribute.c `parse_checked_attribute`); C23
  `[[deprecated]]` and `[[nodiscard]]` too, with their reason string.
- [x] **A per-directory ratchet** in yasos (`scripts/safe_mode_ratchet.py`,
  floor in `scripts/safe_mode_ratchet.json`, CI smoke step): `-fsyntax-only`
  in the safe default over our C directories, warning and error counts may
  only go down. Floor at the start (2026-11-20): 29 directories, 247 warnings,
  186 errors; 38 warnings once regular-C pointers became unchecked.
  - [x] A file stops at its first error -- in nearly every counted file a
    plain pointer -- so the checks after it (and the libc annotations) did not
    move the counts. Error recovery (2026-11-20, `frontend/gen/core/recover.c`,
    test_max_errors.py): the parse goes on after an error up to
    `-fmax-errors=N` errors (8 by default, 0 = no limit; then "compilation
    terminated due to -fmax-errors=N." as gcc); the compile still fails (exit
    1, no object, no link). Two recovery points, a setjmp each: the file-scope
    declaration loop (skip to the declaration's ';', out of the struct bodies
    it is in, or past a function body) and each function body's parse (skip
    the statement to its ';' or its block's '}', out of statement expressions
    and struct bodies; then the rest of the body statement by statement, the
    compound statements still open closed with the locals they declared,
    without code or flow checks). The frontend's nesting state, the replays an
    error left (an inline expansion, a defer body: unwound to the token they
    began at) and the inline/defer bookkeeping are put back; `#pragma
    diagnostic` state stays. From the first recovered error on, the unit goes
    on as `-fsyntax-only`. Errors that do not stop the parse at all: "plain
    pointer in yasc" (the pointer is then `view maybe`, its flow checks
    silent), a warning made an error (`-Werror`, the safe default's errors),
    `#error` (gcc), which `tcc -E` keeps going after too. Still fatal: inside a preprocessor directive (a missing
    include), a macro's substitution or inline asm, an internal error, an error
    while a function's parameters are set up or its code generated, the end of
    the input inside a body. An undeclared name is reported once per function;
    an error a replay says again (an inline expansion, then the function's own
    compile) once; names in skipped tokens count as used (-Wunused), a label a
    skipped part defined is not reported. Same diagnostics at every -O and with
    -fsyntax-only. Cost without errors: a setjmp per function body, a counter
    per macro substitution and per struct body, three stores per scope and per
    begin_macro.
    - [x] A struct or union body goes on with the next member after an
      error (`RECOVER_STRUCT`, one setjmp per struct body while recovery is
      armed): the struct keeps the members that were right and the
      declarators after its body are declared (2026-11-21).
    - [x] A declaration with an error in its declarator still declares its
      name (`int y[ ;`, gcc declares what it can): decl() tells type_decl
      (`recover_decl`), which records the name with the type read so far --
      an old-style function when '(' followed it, an array when '[' did, a
      typedef stays one; at file scope after the file point's pops, in a body
      at the scope it was in, nothing when the name was declared there before
      (test_max_errors.py `*declarator*`, 2026-11-21).
    - [x] Replays go on after an error (`replay_recovering`, a
      `RECOVER_INLINE` point): an error in an inline expansion is the
      callee's -- the expansion ends, the callee is not expanded again and
      the caller goes on after the call, as at -O0 (the rest of the statement
      was lost before); a defer copy ends there and its hidden names and
      break / continue come back; a defer whose exits all come after an error
      is checked once where its names are; a static's initializer saved and
      replayed at the end of the unit, a dropped one's check, and an unsized
      array's initializer (parsed twice from a copy, which ended the unit at
      the copy's end) go on with the next; after an error the inline bodies
      whose calls generated nothing are parsed too; an error in an
      initializer's braces no longer ends the body at the end of the file;
      the end of the input in a body says gcc's "expected declaration or
      statement at end of input" (it was silent). A crash of the file point
      on a helper symbol (`VT_ASM_FUNC` looks like an enum constant to
      `sym_scope`) is fixed (test_max_errors.py, 2026-11-21).
    - [x] `-fmax-errors=0` is no limit (test_cap). The yasos ratchet
      (`scripts/safe_mode_ratchet.py`) should pass `-fmax-errors=0`, or a
      file with more than 8 errors counts 8.
    - [x] At -O1+ a body saved as tokens gives -O0's errors when the lexer
      fails or the input ends in it: `save_function_body` arms a jmp_buf
      around the save (one setjmp per saved body); the end of the input, or
      an error the lexer cannot read past (an unterminated comment or string:
      fatal at every -O now, `lex_error_stuck`), cuts the body there and the
      saved part is parsed at once (check only), the lexer's error raised by
      a `TCC_PCH_REPLAY_ERROR` marker where the parse reaches it. Same
      diagnostics at -O0..-Os and with -fsyntax-only
      (test_max_errors.py test_cut_body_same_at_every_level, 2026-11-21).
      Left: a number's error ("invalid suffix") is still said when the body
      is saved; the errors of the bodies deferred to the end of the unit
      (statics, initializers) come after the others at -O1+, in another
      order than at -O0 (the same set).
- [x] **`#pragma GCC system_header`**: honoured (also through `_Pragma`): the
  rest of the file is `sys`, as a header found on a system include path, and
  so are the `"..."` includes next to it; the lines before it are not; in the
  main file it is ignored with a warning, as gcc does. Used to report
  "#pragma GCC system_header ignored" under `-Wall` for every libc header
  that says it -- 2770 lines in one rootfs build. System headers are also
  recorded by name and line (`tcc_sys_mark_file`), so a body replayed after
  its file was read -- an inline expansion, a deferred function, an inline
  function generated at the end of the unit -- stays quiet too
  (test_system_header_pragma.py).
  - [x] Every warning is quiet in a system header, as gcc (2026-11-20): the
    older -Wall set too (int-conversion, incompatible-pointer-types,
    discarded-qualifiers, implicit-function-declaration, return-type, ...),
    decided once in error1 for ERROR_WARN, before -Werror or the safe default
    make an error of it; errors and `#warning` are still given.
    `-Wsystem-headers` (not in -Wall, as gcc) gives them again; it also makes
    `tcc_in_sys_header()` answer no, so the checks asking it speak there.
    The report-once layer (`tcc_warning_once`, `tcc_fe_warning`, `attr_warn`)
    leaves the decision to error1 too, so -Wsystem-headers reaches every
    warning (test_system_header_warnings.py, test_report_once.py).
  - [x] `tcc -E` writes gcc's line markers (2026-11-20): `# N "file" 1|2`
    and 3 in a system header; `#pragma GCC system_header` is done by -E and
    its line dropped, as gcc. Compiling the output reads them back: flag 3
    makes the text a system header from that line, and flags 1/2 bring a
    file's yasc dialect back where a marker returns to it (a header starts in
    regular C) -- a `-std=yasc` unit's -E output compiles as yasc again
    (test_preprocess_line_markers.py).

## P1: flow checks on the raw IR (step B of the yasc design)

`source/ir/lifetime.c`: one forward dataflow over the raw-IR CFG with per-path
state per variable, then one reporting walk in source order. The frontend
records what the IR cannot say: pointer variables and their kind (owned /
maybe / out / plain), scope ends, return jumps, which call parameters take
ownership and which results are maybe. Inlined bodies are silent (their own
compile reports them).

- [x] **Ownership flow** (`-Wlifetime`): used or handed on after it was moved
  or freed (note: where), freed twice, leaked at a return, at the end of its
  scope (also left by break/continue), at the end of the function, or when
  assigned again. Where paths with different states meet, the message says
  "may". A null test takes ownership out of the zero edge.
- [x] **`maybe`** (`-Wlifetime`): dereferenced where it may be null, a value
  that may be null stored in a pointer that is not maybe or returned from a
  function whose result is not maybe. `if (p)`, `if (!p) return`, `p && ...`,
  `p ? ... : ...`, `while (p)` and a noreturn call on the null path are clean.
  - [x] a maybe struct member (`q->next->v`): each (base pointer variable,
    member offset) reached through `->` or `(*q).` is a path followed like a
    maybe variable ("'q->next' may be null where it is dereferenced"), also
    through another one (`q->next->next`). The frontend records the temp
    holding a `* maybe` member's address (`tcc_ir_lt_note_member`); the
    checker finds the base from the IR. Tests refine it; a store through the
    base sets it; assigning the base, or passing it (or `&q->next`) to a
    parameter that points to non-const memory -- an inlined callee's too --
    forgets it; `const T * view` keeps it; stores through other pointers are
    assumed not to alias. An `owned maybe` member is followed for ownership
    too: used or freed again after `free(q->next)` or after moving it out.
    - [x] a member loaded in one block and dereferenced in another
      (`c ? q->next->v : 0`), and `c ? p->v : 0` for a maybe variable
      (2026-11-20): a temp used in another block than its definition keeps
      what its definitions agree on (alias, base + offset, path, call) and
      its nullness as a state entry per path, so the join of `?:` stored in
      a pointer that is not maybe, or returned, is reported too; a switch
      value's dereference is reported at the switch (test_lifetime.py
      ternary_*, store_before_ternary, switch_value).
    - [x] bases that are not pointer variables (2026-11-20): a local struct
      (`s.next`, by its frame range), a global struct (`g.next`), a global
      pointer's value (`gp->next`). Globals are assumed unchanged by calls
      and are not followed inside an inlined body, so the result is the same
      at every -O (test_lifetime.py OBJ_*).
      - [x] an array element at a constant index (2026-11-20): `a[1].next`
        of a local or global array of structs, `p[1].next` through a pointer
        (a path per element, named so; test_lifetime_paths.py element_*,
        pointer_element, global_element).  Not followed: a call result
        (`f()->next`, a new value per call); `n = f(); n->next` is followed
        (call_result_copied).
      - [x] an element at a variable index (2026-11-20): `a[i].next` of a
        local or global array, `p[i].next` through a pointer.  The raw IR
        computes the address again at each use (`i SHL #k` / `i MUL #size`
        added to the base, through copies and widenings of i a few
        instructions back); the path is (base, member offset, index
        variable) and is forgotten when the index variable is written, so
        `if (a[i].next) a[i].next->v` is clean, untested it may be null
        (test_lifetime_index.py member_*).  a[i] and a[j] (or a[1]) are not
        assumed to be the same element: a store through one does not change
        the other, as stores through other pointers.  Not followed: `a[i +
        1].next`, an index that is not an int local or parameter, `i == j`
        known from `j = i`.
    - [x] copying the member into a variable links them (2026-11-20): `n =
      q->next; if (n) q->next->v` and `if (q->next) n->v` are clean while
      neither changed (a state entry per linked variable; any other value
      in n, a store to the member, forgetting it, or n's scope end breaks
      it; test_lifetime.py copy_*).
  - [x] a maybe value passed to a parameter that is not maybe (declared in
    yasc): direct calls, calls through a pointer, and inlined calls (the
    argument stored into the callee's parameter). Regular-C parameters are
    `view maybe` and take anything.
- [x] **`out` parameters** (`-Wlifetime`): written on every path before
  `return`, never read before the write. Any write through the pointer counts,
  a member's too, and so does handing the pointer on.
- [x] **A borrow outliving its owner** (`-Wlifetime`): a pointer that owns
  nothing, assigned from an owned variable (a copy, or `p + n`), used after the
  owner was freed or moved ("'v' is used after 'p', which it borrows from, was
  freed", note: where). Assigning it again ends the borrow; one borrowing from
  several owners is not followed.
  - [x] **`view` does not escape; `borrows(src)` says where a pointer points**
    (syntax accepted 2026-11-20; all three steps done 2026-11-20).
    Done: the syntax on a function's result (`borrows(a, b)`, `borrows(static)`,
    `borrows(static, s)`, with `maybe`; `borrows(static)` on any pointer), the
    elision and its error, and the flow check (`lifetime.c`
    tcc_ir_view_escape_check, -Wlifetime): a bit per view parameter through
    copies, `p + n`, `c ? a : b` and calls whose result borrows from it;
    reported at a return the result does not allow and at a store into a
    global or through a parameter. Tests `test_yasc_view.py`; libc's
    `sys/yasc/` results say `borrows(static)` (getenv, localtime, getpw*,
    dlerror, setlocale, ...) or their argument (strstr, fgets, localtime_r).
    Then: a pointer read through a view (`n->next`) keeps its source -- the
    IR marks the temps a LOAD/ADD/ASSIGN gives a pointer (ir->lt_ptr_temps);
    a store through a view parameter names it, and what points into the same
    object it is stored in is no escape; a view of an owned local stored
    where it outlives the function ("a view of 'p' is stored in global 'g',
    which outlives it") unless the owner moves somewhere lasting; and
    `borrows(member)` members (`struct cache { char * owned buf; char *
    borrows(buf) key; }`): a store into one must point into the same object's
    member, followed through addresses (base + offset, frame addresses,
    folded loads).
    Then (2026-11-20, later): a borrows(member) member is dead once its source
    member is freed or changed ("'key' may be used after 'buf', which it
    borrows from, was freed or changed"); a value kept in a local variable
    before such a store is checked (origin slots joined per path); and
    `struct T borrows(text) { ... }` -- structs whose values borrow, bound
    where used (`struct T borrows(src) f(...)`, the elision, globals must say
    borrows(static), a value stored where it outlives the function must
    borrow static storage; checked by type, test_yasc_struct_borrows.py).
    A borrow through a call's result (`v = strchr(b, 'x'); free(b); *v`,
    also `first(b) + 1`) is checked by the use-after-free check too
    (2026-11-20, test_lifetime.py through_call*).
    Open: struct values are memory copies in the IR,
    so a borrowing struct is not followed through the flow check. Every pointer in yasc says
    exactly one of `owned`, `view` or `borrows(...)`; `maybe` combines with each.
    - `view`: owns nothing and does not escape -- borrowed for the call or the
      scope. Passing it down is fine; returning it, storing it in a global /
      `static`, or in an object that outlives the function (`s->f = p`,
      `*out = p`) is an error: "'p' is a view: it cannot leave the function".
    - `borrows(x)` (`borrows(a, b)`, `borrows(static)`): points into `x`, and may
      live as long as `x` does -- returned when `x` is a parameter or `static`,
      stored in a field that is itself `borrows(...)`, in a global only as
      `borrows(static)`. When `x` is freed, moved or assigned again, the borrow
      is dead: "'line' is used after 'buf', which it borrows from, was freed".
      `realloc(x)` kills every `borrows(x)`.
      ```c
      char * borrows(s) find(char * view s, int c);
      const char * borrows(static) err_name(int e);
      struct cache { char * owned buf; char * borrows(buf) key; };
      struct tokenizer borrows(text) { const char * borrows(text) pos; };
      struct tokenizer borrows(src) tok_init(const char * view src);
      ```
    - Conversions: `borrows(x)` -> `view` always; `view` -> `borrows(x)` only when
      the value is known to come from `x`. A string literal is `borrows(static)`.
    - Elision: a result written `view` with exactly one `view` parameter means
      `borrows(that parameter)`; with several, `borrows(all of them)`; with none,
      an error suggesting `borrows(static)`. Locals infer their source from the
      flow. `owned` parameters are consumed and never a source.
    - Builds on the borrow check (one owned source today), -Wdangling-pointer
      and the followed globals: each value carries a set of sources (locals with
      their scope, parameters, owned variables, static) checked at stores into
      globals / the heap / through parameters and at returns. libc: `borrows(s)`
      on strchr, strstr, strrchr, strpbrk, strtok_r, memchr, basename, ...
    - Order: syntax and the "view does not escape" rule first; then `borrows`
      through calls; fields `borrows(field)` last (assignments of a field through
      pointers have to be followed).
- [x] **`-Wuninitialized` / `-Wmaybe-uninitialized`**: plain C too; on in the
  safe default, not in `-Wall`. A declaration in a loop body makes the variable
  uninitialised again each iteration. 32 variables at a time as bit masks.
- [x] **`-Wreturn-local-addr`** (`-Wall`): `return &x;`, `return buf;`,
  `return &s.m;`, a parameter's address, a compound literal.
  - [x] `-Wdangling-pointer` (safe default): a frame address stored through a
    parameter, or left in a global when the function returns (a global
    cleared on every path before returning is fine; main is exempt); returned
    through a pointer variable: "may return the address of a local variable
    (held in 'p')" under -Wreturn-local-addr. `if (p == &x)` splits the paths.
- [x] **`-Wnull-dereference`** (safe default, not -Wall): the second kind,
  after optimisation (`tcc_ir_null_deref_check`, after the regalloc pipeline):
  a memory access at address 0, or through a value known in its block to be
  null plus a constant offset. A literal non-zero address (MMIO) is left alone,
  so a member through null whose address the optimizer folded into a literal
  is found at -O0 only.
- [x] **`owned(dealloc)`: ownership that names its deallocator** (accepted
  2026-11-20, done 2026-11-20: the owned node's vreg, tcc_ir_dealloc_check,
  test_yasc_dealloc.py; a plain owned variable takes on the deallocator of
  what it holds, plain `owned` itself is never compared). `FILE * owned(fclose) maybe fopen(...)`;
  plain `owned` is the malloc family (`free`). Handing it to another
  deallocator is an error: "'f' is released by fclose, not free". The gcc
  attribute `malloc(fclose, 1)` maps to it for regular-C headers.
- [x] **`owned(close)` handles for file descriptors** (accepted 2026-11-20, done
  2026-11-20: SYM_HANDLE nodes on int types, LTV_HANDLE, test_yasc_handles.py;
  libc sys/yasc fcntl/unistd/socket, close takes a maybe handle as free
  takes null): `int owned(close) maybe fd = open(...)`. Ownership for ints, only
  where written (a plain int stays plain). `maybe` on an int handle: may hold the
  invalid value (< 0); `fd < 0` / `fd == -1` / `fd >= 0` refine it as a null test
  does a pointer, so the failure path owes no close, and a maybe fd passed to an
  `int view fd` parameter warns. `int view fd` borrows (read, write, ...), and
  does not escape like a pointer view. `int close(int drop owned(close) fd)`.
- [x] **`takes(p)` on a result** (accepted 2026-11-20, replaces the proposed
  `drop_on_success`; done 2026-11-20: YASC_TAKEN on the parameter, the
  caller's p "may have moved" until the result's test, the callee's null
  return leaves p; test_yasc_takes.py; libc realloc, fdopen): the result takes over what `p` owned --
  not necessarily the same pointer (realloc may move the block, fdopen wraps
  the fd in a FILE). With a `maybe` result, success is the result's own test:
  valid -> `p` was consumed; null / < 0 -> `p` is untouched and still the
  caller's. Without `maybe`, `p` always moves.
  ```c
  void * owned maybe takes(p) realloc(void * owned maybe p, size_t n);
  FILE * owned(fclose) maybe takes(fd) fdopen(int owned(close) fd, const char * view mode);
  ```
  - Caller: `q = realloc(p, n); if (!q) { free(p); ... }` is clean; using or
    freeing `p` on the edge where `q` is valid is an error; `p = realloc(p, n)`
    reports that p leaks if realloc fails.
  - Callee: every path returning a valid result must have moved `p` into it
    ("'p' leaks: takes(p) says the result holds it"); every path returning
    null must leave `p` untouched ("returns null but 'p' was freed").
  - Pairs with `borrows(s)` on results (points into s / takes p over).
  - Status codes (`int push(..., void * owned item)` returning 0 when it took
    item) are not covered; in yasc such functions return an error union.
- [x] **Explicit `move(p)` and `borrow(p)`** (accepted 2026-11-20, function
  form; done 2026-11-20: `type/assign_check.c` yasc_move_borrow /
  yasc_verify_move, tests `test_yasc_move.py`, `yasc_move.c`). `move(p)` is an
  owned result (as a call's: discarded it leaks, in a view it is never
  freed), `borrow(p)` a view the flow checks follow as a borrow; both take an
  owned pointer. A name `move` / `borrow` in scope wins (as `error` does).
  The warning is `-Wimplicit-move`, on by default, yasc only. Gaps: an owned
  variable passed to a regular-C (unchecked) parameter needs no move and the
  flow check treats it as a borrow; a struct copied whole with owned members
  is not looked at. Where ownership leaves an owned variable it says so:
  `char * owned q = move(p);`, `take(move(p));`, `n->text = move(text);`,
  `auto q = move(p);`. Without it: "ownership of 'p' moves here: write
  move(p)". `move` of a pointer that owns nothing is an error.
  - Not required (allowed): an argument for a `drop` parameter (`free(p)`,
    `fclose(f)`: the deallocator destroys it, its name says so), and `return
    p;` of a whole local or parameter (it ends there anyway). Required for an
    `owned` parameter that is not `drop` (the value lives on elsewhere), and
    when moving out of a member or a global (`return move(s->buf);`: s lives
    on with the member moved).
  - Borrows: an argument for a `view` parameter is borrowed implicitly -- the
    most restrictive passing (the caller keeps ownership, the callee may
    neither free nor keep it), over by the next line. A borrow that lasts
    (a local, a field) shows in its type (`char * view v = p;`, `borrows(...)`)
    or as `borrow(p)`: `auto v = borrow(p);`, `c->key = borrow(c->buf);`.
    `auto q = p;` with p owned is an error: "write move(p) or borrow(p)".
  - Summary:

    | where the value goes | needs |
    |---|---|
    | argument for a `view` parameter | nothing: implicit borrow (accepted) |
    | argument for a `drop` parameter (`free`, `fclose`) | nothing (`move` allowed) |
    | argument for an `owned` parameter (not `drop`) | `move(p)` |
    | `owned q = p;`, an owned field `= p` | `move(p)` |
    | `return p;` (a whole local or parameter) | nothing (`move` allowed) |
    | `return s->buf;` (a member, a global) | `move(s->buf)` |
    | a lasting borrow in a local or a field | its type (`view` / `borrows`) or `borrow(p)` |
    | `auto q = p;` with p owned | error: `move(p)` or `borrow(p)` |
- [x] **C23 `auto` (and GNU `__auto_type`): the type from the initializer**
  (`decl.c decl_auto_infer`, tests `test_yasc_auto.py`, `c23_auto.c`).
  `auto` with no type specifier infers in C23 (-std=c23/gnu23, -std=yasc, the
  safe default) and in a yasc file; before C23, or with a type (`auto int`),
  it stays the storage class. `__auto_type` infers everywhere. The type is the
  initializer's after lvalue conversion (arrays and functions decay, top-level
  qualifiers go; `const auto` / `static auto` add theirs); the initializer is
  read once unevaluated for its type, as typeof does, then compiled as if the
  type had been written, so it runs once. Locals, statics, file scope and
  `for (auto i = 0u; ...)`. Errors: no initializer, a declarator other than a
  plain name, a braced initializer, a second declarator, void, typedef, and
  `auto` where no initializer can follow (parameters, members, type names).
  In yasc the qualifiers come with the type: `auto p = malloc(n)` is owned and
  followed by the flow checks, `auto q = find(k)` is maybe, a view stays a
  view, and it satisfies the plain-pointer rule (nothing written). `auto q =
  p;` with p owned (a variable or a member) is an error: "write move(p) or
  borrow(p)".
  - [x] `auto v = find(buf)` borrowing from buf: v takes the view type, and
    the view escape check follows the call's result to buf (borrows(...)).

- [x] **Resource pairs** through libc annotations, no hard-coded names (libc
  `feat/yasc-attributes` 0ce2dc5 + `feat/yasc-resource-attrs` 2e3d79f,
  `sys/_attrs.h`): malloc / calloc / aligned_alloc / memalign / realloc /
  strdup / strndup -> free; fopen / fdopen / tmpfile / fmemopen -> fclose;
  popen -> pclose; opendir / fdopendir -> closedir; setmntent -> endmntent;
  dlopen -> dlclose. Leaks, double close, use after close and discarded
  results are reported by the existing ownership checks.
  - [x] Wrong deallocator (`free(f)` on a FILE): `owned(dealloc)` above; gcc's
    `malloc(fclose, 1)` maps to it, so libc needed nothing.
  - [x] `realloc`'s correct `q = realloc(p, n); if (!q) free(p);` was a false
    leak report: `takes(p)` above (libc realloc, reallocarray, fdopen).
  - [x] File descriptors: `owned(close)` handles above (libc open, creat,
    openat, dup, socket, accept, mkstemp, inotify_init; close, read, write).
  - [x] Allocation through out-parameters: owned out-parameters (libc getline,
    getdelim, posix_memalign, getaddrinfo/freeaddrinfo).
    - [x] `regcomp`/`regfree` (an object owned as a whole) and `pipe` (an
      array of owned handles), below; `scandir`, `asprintf` and `socketpair`
      do not exist in yasos libc.
  - Not annotated on purpose: `freopen` (returns its argument, the result
    of `freopen(path, mode, stdin)` is dropped), `realpath` / `getcwd` /
    `tmpnam` (sometimes the caller's buffer), `mmap` (fails with `MAP_FAILED`,
    not null: a `maybe` with another sentinel would cover it).

## P1: frontend checks (no SSA, cheap)

- [x] **`-Wformat` / `-Wformat-security`** (`expr/attr_checks.c`):
  `format(printf|scanf|gnu_printf|gnu_scanf, n, m)` in either spelling, on a
  function (or a function-pointer variable). A literal format is checked
  against each argument as it is parsed: the type after the default promotions
  (signedness is not, as with gcc), too few and too many arguments, `*`
  widths, unknown conversions, a trailing `%`; length modifiers hh h l ll q L
  j z t (libc 7d1d079); scanf's pointers, `%[...]`, `%*d`, `%ms`; `m == 0`
  (a va_list) checks the format alone; `%N$` operands are not checked.
  -Wformat is in -Wall (gcc's). -Wformat-security (not in -Wall, as with gcc;
  the safe default): the format is not a literal (no string literal in the
  argument at all) and nothing follows it. **`-Wformat-n`**: `%n` in a literal
  printf format, an error in the safe default (WD_SAFE_ERR), off under
  -ccompat and not in -Wall (gcc has no such warning); a non-literal format's
  `%n` is libc's to refuse. Tests `test_format.py`.
  - [x] `strftime` (every conversion, its flags, width and E/O modifier, gcc's
    table and wording) and `strfmon` (%i / %n take a double, L a long double)
    are checked; `format_arg(n)` (gettext): inside a format argument, a call
    whose argument n is a literal stands for it, so `printf(_("%d"), s)` is
    checked and `_(s)` stays a non-literal for -Wformat-security
    (test_format.py strftime_strfmon_format_arg, 2026-11-20). Not done: a
    conditional format (`c ? _("%d") : "%s"`), whose arms gcc checks both.
  - [x] Two identical warnings on one line were reported once (each was
    reported once per file, line and text: the -O1+ inliner parses a body
    once per call site, and a frontend warning must appear once). The once
    layer (`tcc_diag_once`) now counts the k-th identical warning of a parse
    apart, so both are reported, once each, at every -O, also in a `static
    inline` called twice (verified 2026-11-20: test_format.py
    two_identical_on_a_line_are_two, test_report_once.py).
- [x] **`-Wunused-result`** (`expr/attr_checks.c`): `warn_unused_result` and
  C23 `[[nodiscard]]` / `[[nodiscard("why")]]` (the reason in the message).
  On by default, as with gcc (-ccompat too), and in -Wall. The result dropped
  by an expression statement, a comma's left side, a for clause or a
  non-last statement of a statement expression. A cast to void keeps it, for
  both attributes (clang's rule; gcc does not let `(void)` silence
  `warn_unused_result`). Tests `test_unused_result.py`.
  - [x] `[[nodiscard]]` on a struct, union or enum type applies to every
    function returning it, gcc's wording ("ignoring return value of type
    'struct S', ..."); warn_unused_result on a type means nothing, as gcc
    (test_unused_result.py nodiscard_type, 2026-11-20).
- [x] **`-Wnonnull`** (-Wall, every unit): a null constant passed to a
  `nonnull` parameter (`SymAttr.nonnull`); `returns_nonnull` makes a malloc
  result `owned` rather than `owned maybe`.
  - [x] Map `nonnull` to "not `maybe`" for the flow check too (2026-11-20,
    -Wlifetime): a maybe value passed to a `nonnull` parameter of a regular-C
    header is reported ("... argument 2 of 'cpy', which is declared
    nonnull"); a null constant stays -Wnonnull's, reported once
    (test_lifetime.py nonnull_attribute_*).
- [x] **`-Wdeprecated-declarations`** (`expr/attr_checks.c`):
  `__attribute__((deprecated[("why")]))` and `[[deprecated[("why")]]]` on
  functions, variables, typedefs, struct/union/enum tags and enumerators
  (also `int f [[deprecated]] (void)`). On by default, as with gcc (-ccompat
  too), and in -Wall. Reported at each use, "'x' is deprecated[: why]"; not
  at a redeclaration or the definition, not inside a deprecated tag's own
  body, and not where a declaration whose specifiers say deprecated names a
  deprecated type (in the specifiers or its declarators), as with gcc. Tests
  `test_deprecated_declarations.py`.
  - [x] A deprecated struct member is reported at each use (not in a
    designated initializer, as gcc; after a declarator the attribute is only
    its own), and every warning has gcc's "note: declared here" at the
    declaration, in its own file (test_deprecated_declarations.py
    struct_members, declared_here_note, 2026-11-20).
  - The libc plan (`deprecated` on `tmpnam`, `sprintf`, `strcpy`, `strtok`,
    `atoi` in `sys/yasc/`) is not in libc yet. Once it is, every yasc-mode
    call to them in our C directories is a warning the ratchet counts (it runs
    the safe default, which is yasc): each directory using them goes up and
    the ratchet fails until the calls go or the floor is raised. Today most
    counted files stop at their first error (a plain pointer) before any call,
    so the first effect is small and grows as files reach yasc.
- [x] **`-Wunused-variable` / `-Wunused-parameter` / `-Wunused-function`** for
  statics, **`-Wunused-but-set-variable`**, and **`-Wunused-value`** (`x == 1;`).
  Honour `[[maybe_unused]]`, `__attribute__((unused))` and `(void)x`.
  (`frontend/gen/core/unused.c`, `tests/ir_tests/test_unused.py`.) In -Wall, as
  in gcc's C -Wall, and so in the safe default: unused-variable (locals and
  file-scope statics), unused-const-variable (main file only, gcc's level 1),
  unused-function, unused-but-set-variable, unused-value (gcc's three messages,
  `&&`/`||`/`?:` as gcc), unused-label; `-Wunused` names them. unused-parameter
  and unused-but-set-parameter are -Wextra in gcc: outside -Wall and the safe
  default. Decided from the source, the same at every -O, once each: locals
  are tracked outside inline expansions and reported at their scope's end with
  the declaration's line, a static inline (and a nested function) body's
  mentions are read from its tokens when it is saved, recursion is not a use,
  a later parse of the same body repeats nothing; a body every call inlined
  gets the check-only parse. used / cleanup / alias / defer bodies / nested
  captures / asm goto / `&&label` count as uses; system headers stay quiet.
  - [x] set but not used through a variable index or a member of a local
    (`a[i] = 0`, `s.arr[i] = 1`, `s[i].f = 0`, `s.f = 1` with the address
    in a register): the part is kept by its stack place, as gcc -- not when
    the index names the local, nor through a pointer, nor for ++ / +=.
    Constant elements of a local at a negative frame offset were never
    reported (an unsigned compare), fixed (test_unused.py
    set_but_not_used_through_parts, 2026-11-20).
  - [x] **`-Wunused-local-typedefs`** (-Wall and -Wunused, gcc's): a typedef
    in a block nothing names, at every -O (test_unused.py local_typedefs,
    2026-11-20). `-Wunused -Wextra` implies -Wunused-parameter (-Wextra,
    the unify branch).
  - [x] a statement of a statement expression other than the last
    (`({ a; b; })`) is checked as a discarded value when the next one comes
    (test_unused.py statement_expression_statements, 2026-11-20); gcc reports
    at the macro's definition, tcc at the expansion.
- [x] **`-Wimplicit-fallthrough`** (gcc -Wextra; the safe default, not -Wall)
  plus `[[fallthrough]];` / `[[gnu::fallthrough]];` /
  `__attribute__((fallthrough));`, now statements anywhere (an if or else arm
  too) and `__has_attribute(fallthrough)`. An empty case, a statement that
  cannot complete (return/break/continue/goto, a noreturn call, an if/else
  whose arms all do, `for(;;)`/`while(1)` with no break, a switch with a
  default that never breaks out) and gcc's -Wimplicit-fallthrough=3 comments
  right before the label (`/* fall through */`, `// FALLTHRU`, `/* Falls
  through. */`, ...; tccpp.c marks the label's line) are allowed.
  `tests/ir_tests/test_implicit_fallthrough.py`.
  - [x] A comment read across an I/O buffer refill (8 KB host, 2 KB device) is
    put together from the old buffer's tail; blanks or the label running into
    the buffer's end are finished in the next one; other comments may come
    between the comment and the label, as gcc (test_implicit_fallthrough.py
    comment_across_the_read_buffer, comment_then_other_comments, 2026-11-20).
  - [x] gcc's "attribute 'fallthrough' not preceding a case label or default
    label", unconditional as there (only -w): a statement after it, or the
    switch body's end; an if arm and an inner block's end are left alone
    (test_implicit_fallthrough.py attribute_not_before_a_label).
- [x] **`-Wparentheses`** (-Wall): an unparenthesised `=` as an
  if/while/do/for condition, and gcc's precedence cases (`a && b || c`,
  `a + b << c`, `a | b & c`, `a & b == c`, `a == b == c`, `a < b < c`, ...),
  never in an `#if`; **`-Wdangling-else`** (-Wall, as gcc's -Wparentheses
  has it), also through a loop body; **`-Wempty-body`** (gcc -Wextra; the
  safe default, not -Wall): `if (x);` without else, `else;`, `do ; while (x);`.
  `tests/ir_tests/test_parentheses.py`.
  - [x] `!a == b`: **`-Wlogical-not-parentheses`** (-Wall, gcc's), with its
    note; `!x & y` / `!x | y`: -Wparentheses' "suggest parentheses around
    operand of '!'"; a right side that is a truth value (or 0 / 0-or-1) is
    quiet, as gcc 16 line for line (test_logical_not_parentheses.py, 2026-11-20).
- [x] **`-Wswitch`** (-Wall): a switch on an enum value with no `default` that
  misses an enumerator (equal values count once, case ranges cover), and a
  case value not in the enum. `tests/ir_tests/test_switch_warning.py`.
- [x] **`-Wshadow`** (only when asked: noisy): a local, static local or
  parameter hiding a local, a parameter, or a file-scope object or enumerator
  (not a function or typedef), with a note at the hidden one.
  `tests/ir_tests/test_shadow.py`.
  - [x] A parameter's warning and note give its own line (the declarator's
    parameter lines, kept for -Wunused, are kept for -Wshadow too)
    (test_shadow.py parameter_at_its_own_line, 2026-11-20).
  - [x] A declaration a macro from a system header expands to is quiet, as
    gcc: type_decl notes whether the declared name came from such a macro
    (only while -Wshadow is on) (test_system_header_macro.py, 2026-11-21).
    Not for parameters.

  These frontend checks report once per place at every -O level: they stay
  quiet while the inliner replays a body at a call site, the check-only parse
  of a body every call inlined reports them (check_uncompiled_inline_functions
  runs for them too), and `tcc_diag_first` drops a repeat (a function compiled
  twice; a wrapper over `tcc_diag_once` now, see "One report once mechanism"
  below). A body replayed under its file's name keeps that file's
  system-header status (`tcc_sys_file`). gcc's -Wextra members tcc has are in
  `-Wextra` now (see below); implicit-fallthrough and empty-body are in the
  safe default too.
- [x] **`-Wsign-compare`**, then **`-Wconversion` / `-Wsign-conversion`**
  (none in -Wall or the safe default: gcc keeps sign-compare in -Wextra, the
  others in neither, and all are noisy; `test_sign_compare_conversion.py`).
  sign-compare: a comparison done unsigned that converts a signed operand not
  known non-negative (a constant >= 0, a promoted unsigned char/short/bit-field,
  a comparison result; `x == 3u` is quiet). conversion: assignment,
  initializer, argument and return to a narrower integer, float <-> integer,
  double to float, constants only when the value changes; `c += y` checks y.
  sign-conversion is implied by -Wconversion unless given. gcc's wording.
  - [x] The value ranges gcc's conversion_warning sees (op.c FeNarrow, kept
    by stack place): `x & C` with C fitting the target signed and unsigned,
    `+ - * | ^` of operands narrower than int (constants fitting them),
    comparisons (also in a register), both `?:` arms, unary minus, a call's
    narrow return type; the compiler's own stores (a bit-field's unit, a VLA
    size, a K&R parameter, `c++`) are not checked. tests/ir_tests with
    -Wconversion (2026-11-20): 715 -> 469 warnings, arm-none-eabi-gcc 15 602;
    lines both report 281 of gcc's 422, lines only tcc reports 359 -> 148
    (test_sign_compare_conversion.py value_ranges_as_gcc). Left: `x % 10`,
    `u >> 24` stay reported (gcc too); gcc's -Wfloat-conversion constants and
    a builtin's result type (`__builtin_signbit` is unsigned here) differ.
- [x] **Constant-expression checks** (`test_const_expr_checks.py`), in the
  operators the source writes (`gen_op_checked`), not in unevaluated operands
  (sizeof, the dead arm of a constant `?:`, `&&`, `||`) nor in `#if`; dead
  statements are checked, as gcc. Reported through `tcc_fe_warning`: once per
  line and text however often a body is parsed (inline replays, the static
  compiled at the end, check-only parses), named after the body's own file in
  an inline expansion, quiet in system headers (also replayed: `sys_files`).
  - `-Wshift-count-overflow` / `-Wshift-count-negative` (-Wall)
  - `-Wdiv-by-zero` (-Wall; also `1.0 / 0`, as gcc)
  - `-Woverflow` (-Wall): signed overflow folding two constants, `-INT_MIN`,
    and a constant converted to a type whose range [-2^(n-1), 2^n-1] it is
    outside (`char c = 300`, bit-fields too; `unsigned char u = -1` is
    -Wsign-conversion, as gcc)
  - `-Wtautological-compare` (-Wall): the same variable, parameter or member
    of one compared with itself (no float, no volatile)
  - `-Wtype-limits` (not -Wall, gcc's -Wextra): `unsigned < 0`, `>= 0`, a
    char/short against a constant out of its range
  - [x] A warning in the expansion of a macro a system header defines is
    quiet, as gcc, unless -Wsystem-headers; errors are always given
    (2026-11-21, test_system_header_macro.py). Such a macro is `MACRO_SYS`;
    next() marks its live expansion (`TokenString.sys`, no token added). A
    body saved as tokens keeps the ranges of those tokens in a side table by
    the string's address (tccpp.c `tok_str_sys_*`: moved by tok_str_keep,
    dropped by tok_str_free, copied with the string), never in its tokens:
    saved bodies, token budgets and objects are unchanged (all of
    tests/ir_tests and half the gcc torture sources at -O0..-Os compared
    with fd6bb2a565). Only a warning about to be given looks: the position
    read last (error1 without a line, the once-layer with at_line 0) is in a
    live expansion, or in a range of the replayed string. Same diagnostics at
    every -O. Approximations: the whole expansion counts, also a user's
    argument in it (gcc speaks about an argument's tokens), and a user's
    macro that expands a system one does not.
    (Marks in the tokens, tried 2026-11-20 on feat/rest-fe, lengthened the
    saved bodies the inliner sizes by tokens and changed objects.)
  - [x] `-Wtautological-compare`: two operands that read the same tokens with
    no side effect between them (`p->a == p->a`, `(x+1) == (x+1)`; next()
    keeps a running token hash while the check can be wanted), and gcc's
    bitwise forms `(x & 1) == 2`, `(x | 4) != 1`; **`-Wshift-overflow`**
    (-Wall; gcc's level 1, on by default there): `2 << 31`, gcc's wording
    (test_const_expr_checks.py, 2026-11-20). Unlike gcc: no commuted operands
    (`a + b == b + a`), and a comparison written in a macro expansion is
    reported (gcc is quiet there) -- a saved inline body has lost which
    tokens came from a macro.
  - [x] A static function whose every call is folded by evaluating its body
    at compile time (constant arguments, -O1+; from a header it was never
    checked) gets the check-only parse at the end of the unit with all its
    diagnostics on (InlineFunc.folded; test_const_expr_checks.py
    static_folded_at_compile_time_is_checked, 2026-11-20).
  - Fixed on the way: `DATA_ONLY_WANTED` was 2 in backend/generators/function.c,
    so after every function the file-scope code ran with a no-eval count of 2.
- [x] **`-Warray-bounds`** (frontend part, -Wall, not -O dependent;
  `test_array_bounds.py`): a constant subscript of a known-size array, negative
  or past the end; `&a[n]` is fine, `a[n]`, `&a[n][0]`, `&a[n+1]` warn. Trailing
  array members count as flexible unless they have >= 2 elements and the struct
  is a named object (gcc); unions, `[]`, pointers, sizeof and dead code never.
  - [x] Pointer arithmetic with constants on such an array counts as its
    subscript: `*(a + 4)`, `(a + 2)[3]`, `(a + 4)->m`; `&*(a + 4)` is the end
    (test_array_bounds.py, 2026-11-20). A pointer variable holding `a + 4` is
    not followed (that is flow, -O2 in gcc).
- [x] **`-Wsizeof-pointer-memaccess` / `-Wsizeof-array-argument`** (-Wall;
  `test_sizeof_checks.py`): the length of memset/memcpy/memmove/memcmp/strncpy/
  strncat/strncmp/snprintf/bzero/... (by name) is exactly `sizeof p` of a pointer
  argument, or of its pointer type, with gcc's messages; `sizeof` of a parameter
  declared as an array (`SymAttr.param_array`).
  - [x] "The same expression" is also the same tokens (`sizeof s->p` with
    `s->p` passed, either one in parentheses), through the token hash of
    -Wtautological-compare (test_sizeof_checks.py, 2026-11-20).
- [x] **`-Wstrict-prototypes` / `-Wmissing-prototypes`** (test_prototypes.py).
  C23 (`-std=c23`, so the safe default) and yasc files now read `()` as
  `(void)` -- calling `int f();` with arguments is an error there; C17 and
  before keep the old-style meaning. `-Wstrict-prototypes`: a declarator
  without parameter types (`int f()` before C23, in declarations, typedefs,
  function pointers, casts) and an old-style definition not preceded by a
  prototype; the safe default (only old-style definitions are left there),
  not -Wall, as gcc. `-Wmissing-prototypes`: "no previous prototype for 'f'"
  for a global function (not main, static, inline or a yasc module's export);
  neither -Wall nor the safe default: 815 reports over our C under
  `-ccompat`, 615 of them in yasvi's test files (145 in today's safe-default
  ratchet, which stops each file at its first error) -- turning it on in the
  safe default would raise the ratchet floor by those. The `()` change and
  -Wstrict-prototypes leave today's ratchet counts unchanged.
- [x] **`-Wpointer-arith`** (test_pointer_arith.py): arithmetic on `void *` and
  on function pointers (`+ - ++ -- +=`, a difference), `sizeof(void)`,
  `sizeof` of a function, `_Alignof(void)`. Outside -Wall and the safe
  default (gcc: -Wpedantic only; 45 reports over our C, 4 in the ratchet).
- [x] **`-Wcast-qual`** (test_cast_checks.py): an explicit cast dropping
  const/volatile from the pointed-to type at any level, or adding const below
  a level that is not const (`char **` to `const char **`). Outside -Wall and
  the safe default, as gcc (33 reports over our C).
- [x] **`-Wcast-align`** (test_cast_checks.py): an explicit pointer cast that
  raises the target's alignment -- a char buffer or a packed struct cast to a
  pointer to a word (M23 faults on every unaligned access, M33 on LDRD/LDM).
  As gcc's `-Wcast-align=strict` (plain gcc `-Wcast-align` does nothing on a
  target with unaligned access), never from `void *` (malloc's result, an
  unknown target), nor to or from an incomplete type. Outside -Wall and the
  safe default: 103 reports over our C, mostly buffers known to be aligned.
- [x] **`-Waddress-of-packed-member`** (-Wall, so the safe default; gcc has it
  on by default; test_address_of_packed_member.py): the address of a member a
  packed struct or `#pragma pack` places below its type's alignment (also an
  array member decaying, and a member of a packed member) converted --
  assigned, passed, returned, cast -- to a pointer whose target needs more;
  char and void pointers take it as it is, an explicit `(void *)` cast hands
  it on, as gcc. Matches arm-none-eabi-gcc 15 line for line on the test;
  tinycc's own source and our C have no hit.
  - [x] `&p->arr[i]` and a member reached through an element of a packed
    array member (`&p->items[i].x`) are followed, the message naming the
    packed struct, as arm-none-eabi-gcc 15 (test_address_of_packed_member.py
    elements_of_packed_array_members, 2026-11-20).

## P1: stack, YasOS-specific

Process stacks are 4 to 8 KB, and stack overflows have been real bugs
(toybox, VFS paths, the shell).

- [x] **`-Wframe-larger-than=N`** (2026-11-20): "the frame size of 'f' is
  5024 bytes, larger than 4096 bytes", at the line the body starts, after the
  function is generated. The frame is the real drop below the caller's SP --
  the prologue's PUSH (saved registers, LR, r0-r3 of a variadic) and SUB SP
  (locals, spills, the outgoing argument area) plus the most the body drops SP
  further (scratch PUSHes, soft-float helper windows, a >256-byte call's own
  argument window): the backend reads it off the instructions it emits, without
  changing them (`arm-thumb-emit.c` `th_su`, `backend/generators/stack_usage.c`).
  Same on the -O0 fast path and at -O1+; the number is the frame of the code
  generated, so it depends on -O (register allocation, inlining) -- accepted.
  Not in -Wall nor the safe default (gcc agrees). `-Werror=frame-larger-than=N`,
  `-Wno-frame-larger-than`, `#pragma GCC diagnostic ignored "-Wframe-larger-than="`.
  A function recompiled at the end of the unit (late reopt) warns at most once.
  Test: `tests/ir_tests/test_stack_usage.py`.
  - [x] Locations as gcc (2026-11-20): -Wframe-larger-than / -Wstack-usage
    at the body's closing brace, the `.su` (and the `.ci`'s FILE:LINE) at the
    name's line and 1-based byte column. type_decl notes where the declared
    name was read (only while a stack option is on; the reader's buffer
    refills keep the line's column, `BufferedFile.col_base`), and the
    definition files it by its Sym, so a body replayed at the end of the unit
    keeps it; a name a macro made gets the expansion's line and column 1.
    Matches arm-none-eabi-gcc 15 on test_stack_usage.py's LOC sample (tabs,
    `int\nf(...)`, `int (f)(...)`, a function returning a function pointer,
    a static inline whose address is taken) at every -O.
- [x] **`-Wvla` / `-Walloca`** (2026-11-20): "variable length array 'a' is
  used" at a VLA object (also a struct with a VLA member), "use of 'alloca'" at
  alloca / `__builtin_alloca`; frontend, the same at every -O (once, also for a
  static inline every call inlined: the check-only parse reports it, the
  expansions do not). A VLA parameter takes no stack and is not reported (gcc
  does). Quiet in system headers. Not in -Wall (nor gcc's); **in the safe
  default** (WD_SAFE, warnings): YasOS stacks are 4-8 KB and these are the
  frames no limit bounds; nothing in the ratchet's directories trips them
  (measured with and without `-ccompat`), and tinycc builds with `-ccompat
  -Wall`, which does not include them. Test: `tests/ir_tests/test_vla_alloca.py`.
- [x] **`-fstack-usage`** (2026-11-20): `<object>.su` beside each object, gcc's
  format `file:line:col:function<TAB>bytes<TAB>static|dynamic,bounded|dynamic`
  (`-c -o x.o` -> `x.su`; compiling and linking at once -> `<source>.su` in the
  current directory, as gcc before 11). `dynamic,bounded`: SP moves inside the
  body by a known amount, already counted; `dynamic`: VLA or alloca.
- [x] **Call-graph max-stack tool** (2026-11-20): `-fcallgraph-info[=su]`
  writes `<object>.ci` (format of our own, in `scripts/stack_usage.py`'s help:
  `function NAME FILE:LINE BYTES QUAL [local]`, then `call` / `tailcall CALLEE`
  for every BL / B.W relocation of the body -- libtcc1 and libc helpers the
  backend calls included -- and `indirect N`). `scripts/stack_usage.py` reads
  the .ci files of a program's objects or of a directory tree and sums the
  worst path per entry point (`main`, or `-e NAME`...); recursion, calls
  through a pointer and dynamic frames make it UNBOUNDED, a callee no file
  describes makes it a lower bound ("unknown") unless `--unknown-budget N` /
  `--budget NAME=N` / `--indirect-budget N` give it a cost; `--limit N` exits
  1 above N, unbounded or unknown. A tail call costs the callee alone; `local`
  (static) functions resolve within their own object first. Over tinycc's own
  sources it reports main unbounded (recursive descent, the pass table's
  indirect calls) with a 24.8 KB deepest bounded path.
  - How YasOS would use it instead of guessing: build the kernel-tcc C
    (`zig build kernel-tcc-plan`) and the apps with `-fstack-usage
    -fcallgraph-info`; for each `RankedMutex` with `stack_needed`
    (`fs_lock.zig` 1536, `ext4fs.zig` 2048, the page pool 1024) run
    `stack_usage.py -e <every function that takes the lock>` with
    `--budget` for the assembly helpers, and make the margin the result (a CI
    step with `--limit` keeps it true); the VFS's indirect calls need their
    targets as budgets or `--indirect-budget` -- a reported UNBOUNDED is the
    recursion or callback to look at, which `stack_overflow_sweep.py` only
    finds by hitting it. For an app, `-e main` gives the YAFF `-stack-size`.
  - [x] `.ci` for libtcc1 and libc (2026-11-20): `lib/Makefile` builds
    libtcc1's members with `-fcallgraph-info=su` and gathers them into
    `armv8m-libtcc1.ci` beside `armv8m-libtcc1.a` (`object NAME` sections,
    a local function stays in its member); libc branch `feat/callgraph-info`
    (on fix/crt1-unused-fini-count) does the same for `build/libc.ci`.
    Neither is installed: they are host-side data for stack_usage.py and stay
    in their build trees, never in a target image (decided 2026-11-20).
    Objects, archives and libc.so byte-identical with and without.
    `stack_usage.py` takes an archive (its `.ci` beside it, e.g.
    `../libc/build/libc.a`) and `--libtcc1` (the tree's).
    Still undescribed: the assembly members (`__aeabi_ldivmod`, memcpy, setjmp,
    vfork) and the fp libraries -- `--budget` them.
  - [x] Indirect calls resolved (2026-11-20): each call through a pointer
    records its function type (the frontend's, by call_id, so an inlined
    body's calls keep theirs) and each defined function its own, as a calling
    shape (`i` = any pointer / integer up to 32 bits / enum / _Bool, `l`, `f`,
    `d`, `v`, `sN`; `RET(?)` unprototyped); every function a section refers to
    other than by a branch is `address-taken` (relocation scan at the end of
    the unit: data tables, literals, MOVW/MOVT; not debug or unwind). `.ci`
    format 2: `type SIG`, `indirect N SIG`, `address-taken NAME [local]`.
    `stack_usage.py` takes the worst address-taken function of the call's
    shape (recursion through it UNBOUNDED, an address-taken name with no
    record UNKNOWN, no candidate UNKNOWN "(indirect SIG)", a type tcc did not
    see UNBOUNDED); `--indirect-unbounded` restores the old behaviour,
    `--indirect-budget` prices the unresolved ones; version 1 files fall
    back to it. Sound only without: casts to another shape, addresses taken
    in assembly or in units not given (callbacks from undescribed
    libraries), indirect branches in assembly (documented in the script's
    help). libc's qsort reaches a program's static comparator. Test:
    test_stack_usage.py (ci_indirect_types, script_resolves_indirect_by_shape,
    recursion/no candidate, version 1, archive sections, libtcc1).
- [x] **`-Wstack-usage=N`** (2026-11-20): "stack usage of 'f' is N bytes,
  larger than L bytes", and "stack usage of 'f' might be unbounded" for every
  dynamic frame (VLA, alloca), whatever its size. Not in -Wall nor the safe
  default (gcc agrees). Test: `tests/ir_tests/test_stack_usage.py`.
- Byte-identity: the objects of every `tests/ir_tests/*.c` at -O0/-O1/-O2/-Os
  (also with -g, -ffunction-sections) are identical with and without all of
  these options (`test_stack_usage.py::test_objects_byte_identical` keeps a
  sample).

## P2: object sizes and buffer overflows

- [x] **`__builtin_object_size` / `__builtin_dynamic_object_size`**
  (2026-11-20, `frontend/gen/builtin/objsize.c`): gcc's types 0-3 ((size_t)-1
  / 0 when unknown), decided by the parser as the argument is read, so the
  answer is the SAME at every -O (gcc's grows with optimisation; ours never
  does): declared objects (locals, parameters' addresses, globals, string and
  compound literals), members as the closest subobject (`.`, `->`, `[]`, `&`,
  casts, pointer +/- an integer; a trailing array through a pointer or of <= 1
  element is flexible), and allocations -- `alloc_size` (now a checked
  attribute) and malloc/calloc/realloc/aligned_alloc/memalign/alloca by name
  (not with -fno-builtin) with constant sizes, directly or through a local
  pointer variable every store so far gave one or null (sizes merge; not
  followed in a loop or while/for header begun after its declaration unless it
  is const, after a goto label, once `&p` is taken or anything else stored --
  `SymAttr.possibly_written`, unused for locals before).  Such a variable also
  follows the addresses of declared objects, alloca blocks and conditionals of
  known arms stored to it (the right-hand side is parsed in the object-size
  context; gcc's builtins/*-chk.c test3 needs it at -O1/-O2).  Stores merge,
  they never kill: `p = buf; p += 2` gives 20 for type 0 and 18 for type 2, and
  no member subobject is followed through a variable.  Side effects in the
  argument (an assignment, ++, a call) make it unknown, as gcc says.
  Parameters are never looked at, also in an inline expansion.  The dynamic one adds VLAs and
  `malloc(n)` / `calloc(n, k)` while the variable or parameter n has no store
  since.  `__has_builtin` says 1.  Tests: `test_object_size.py`,
  `objsize_builtin.c` (run, every -O prints the same).
  **What libc needs for `_FORTIFY_SOURCE`** (not done here): `alloc_size` on
  malloc (1), calloc (1, 2), realloc (2), aligned_alloc/memalign (2) -- tcc
  knows these by name already, but other compilers do not; the `__*_chk`
  functions and `__chk_fail`; header wrappers under `#if _FORTIFY_SOURCE > 0 &&
  __has_builtin(__builtin_object_size)` that call
  `__builtin___memcpy_chk(d, s, n, __builtin_object_size(d, 0))` (type 1 for
  the string functions; level 3: `__builtin_dynamic_object_size`).  tcc already
  folds those to the plain call when it can prove the size, keeps the check
  otherwise (`builtin/chk.c`), and warns at a constant overflow.
- [x] **`-Wstringop-overflow` / `-Wformat-overflow` / `-Wformat-truncation`**
  (2026-11-20, objsize.c, all `-Wall`; gcc has the first on by default, the
  others in -Wall): memcpy, memmove, mempcpy, memset, bzero (the whole object),
  strcpy, stpcpy, strcat (a constant source), strncpy, stpncpy, strncat,
  snprintf's bound (the closest subobject; `=1` the whole object) with a known
  length over the destination's size; `__builtin___*_chk` with a constant
  length over its size.  sprintf/vsprintf whose literal format's shortest
  output (constants formatted on the host, unknown values at their shortest)
  plus the nul does not fit; snprintf/vsnprintf whose shortest output does not
  fit the constant bound -- level 1 only when the result is not used, `=2`
  always (`-Wformat-overflow=2` is level 1).  Tests:
  `test_stringop_overflow.py`.
- [x] **`-Walloc-size-larger-than=N` and `-Walloc-mul-overflow`** (2026-11-20,
  objsize.c): a constant size or calloc-style product over N (PTRDIFF_MAX by
  default; SIZE_MAX or more turns it off), `-Wall` (gcc: on by default).
  `-Walloc-mul-overflow` (tcc's name, not in -Wall: `malloc(n * sizeof *p)` is
  everywhere and a test of n before it is not seen): the size argument of a
  one-size allocator is a multiplication with an operand not constant and no
  known bound (constants, the local value facts, a narrow type) keeping it in
  range; suggests calloc / reallocarray.  Tests: `test_alloc_checks.py`.
- [x] **`-Wfree-nonheap-object` / `-Wmismatched-dealloc`** (2026-11-20,
  objsize.c, both `-Wall`): free, realloc or any deallocator a
  `malloc(dealloc, i)` names (in regular C too now) given a local or global
  object, a parameter's address, a literal, or a followed allocation plus a
  nonzero offset; a followed allocation (the call itself, or a variable as
  above) given to another allocator's deallocator, with a note naming the
  allocator (free and realloc release the free family's).  Tests:
  `test_alloc_checks.py`.
- [x] **`-Warray-bounds` (flow part)** (2026-11-20, objsize.c, under
  `-Warray-bounds`): not SSA -- the loop-bound pattern read by the parser, so
  the same at every -O: `for (i = C0; i < N; i++)` (<=, N > i, ++i, i += 1, a
  declared i) with a[i] / a[i + k] of a known-size array in the body, checked
  with i's last value; only for an access no branch precedes in the body (an
  inline expansion's own branches do not count), when the body stores nothing
  to i and does not take its address; &a[n] is fine.  Test:
  `test_array_bounds_loop.py`.
  - [x] the value-range part (2026-11-21, `ir/bounds.c`, `-Warray-bounds=2`,
    not in -Wall; plain `-Warray-bounds` stays level 1): while and do loops,
    other strides, loops counting down, conditions in the body (`if (i < N)
    a[i]`), indexes computed from i, a subscript after a loop.  The frontend
    records each `a[i]` of a known-size array with a variable index; on the
    raw IR (before any pass, so the same at every -O, and only at level 2)
    an interval per followed vreg and per block, narrowed on CMP/JUMPIF edges
    -- a dense analysis needs no SSA, so the "check SSA" is this.  Loops are
    unrolled while the header decides every exit; the first trip that may
    leave on an unknown condition is still checked, the widened rest is
    not.  Reported: a whole range out of bounds on a reached path (gcc's
    text, a note with the values), once; values from inline expansions are
    unknown; calls are taken to return.  Caps: 512 subscripts, 128 vregs,
    8192 blocks, 512 loops, 4096 trips of a loop, 2M instructions visited
    per function.  Unity TUs of tinycc: -O0 +14%, -O2 +3%, no reports; the
    gcc torture suite: 8 files, all real (pr55875 only by "calls return").
    Test: `test_array_bounds_flow.py`.

## P2: runtime instrumentation (the dynamic side)

- [x] **`-fsanitize=undefined -fsanitize-trap`**: a minimal UBSan with no
  runtime (`frontend/gen/op/ubsan.c`, 2026-11-20). The frontend emits each test
  as ordinary IR before the operation and a TRAP (`udf #254`, a UDF stop under
  ptrace) on its failing side, so every -O keeps it and optimises the rest:
  - signed `+ - *`, negation, `++ --`, compound assignment (done in the
    unsigned type, so no "cannot overflow" fold reaches the test), `MIN / -1`;
  - integer division by zero; a shift count `< 0` or `>=` the width; a
    negative or overflowing signed `<<`;
  - a null or misaligned dereference of a computed pointer (alignment capped
    at 4, what LDRD needs and tcc's frames promise);
  - a VLA bound `<= 0`; an index outside an array of known size (`&a[n]`
    allowed; a trailing `[0]`/`[1]` is taken as flexible).

  gcc's names: `-fsanitize=undefined` and `signed-integer-overflow`, `shift`
  (`-base`, `-exponent`), `integer-divide-by-zero`, `null`, `alignment`,
  `vla-bound`, `bounds`; `-fno-sanitize=`; `-fsanitize-trap[=..]` (trap is
  the default mode, see the handler mode below); other sanitizers are an
  error. Not checked:
  operations folded at compile time, pointer arithmetic, conversions. Off,
  objects are unchanged. test_sanitize.py (QEMU, -O0..-Os). A sweep of the
  QEMU suite with it found and fixed two cross bugs (a CMP #0 fused into an
  ADD dropped its barrel shift; a return-tail register share ignored the
  partner's coalesced members) and filed three in docs/bugs
  (union-cast-struct-escaped-address-drops-copy, licm-load-across-builtin-apply,
  computed-goto-sanitized-add-lost).
  - [x] A handler mode calling libc `__ubsan_handle_*` with the source
    location (2026-11-21). Clang's ABI: a record per check in `.data`
    ({file, line, column 0}, type descriptors {kind, info, name} in
    `.rodata`, shared through the string-literal pool; type_mismatch_v1's
    log2(alignment) and kind 0 = load for any access), the operands as
    ValueHandles (a long long through a pointer to a copy); add/sub/mul/
    negate_overflow, divrem_overflow (also division by zero),
    shift_out_of_bounds, type_mismatch_v1 (null, alignment),
    vla_bound_not_positive, out_of_bounds. Decided: trap stays the default
    (tcc ships no runtime and a trap needs none); asking for the runtime's
    behaviour picks the handlers for the checks named --
    `-fno-sanitize-trap[=..]`, `-fsanitize-recover[=..]` (report and go on:
    the plain handlers) or `-fno-sanitize-recover[=..]` (report and abort:
    the `_abort` handlers, followed by the TRAP so that side never rejoins);
    `-fsanitize-trap[=..]` wins over them in any order (clang's rule);
    `-ftrapv`'s checks always trap. Trap-mode and flag-less objects are
    byte-identical to before. The runtime: yasos libc branch
    `feat/ubsan-handlers` (`ubsan.c`, clang's messages, each place once);
    the QEMU tests link its copy `tests/ir_tests/ubsan_runtime.c`.
    test_sanitize.py (reports and lines at -O0..-Os, abort, trap wins,
    record layout).
- [x] **`-fstack-protector`, `-strong`, `-all`** (2026-11-20): a canary right
  below the saved registers (`arm-thumb-frame.c`), copied from
  `__stack_chk_guard` by the prologue and compared by the epilogue, which calls
  `__stack_chk_fail`. gcc's choice of functions, made by the frontend while it
  parses the body; a protected function makes no tail call; naked ones get
  none. `__SSP__`/`__SSP_STRONG__`/`__SSP_ALL__`. The runtime is libc's: newlib
  for the QEMU runs, yasos libc branch `feat/stack-protector` (guard mixed at
  startup by crt1). The QEMU suite passes with `-fstack-protector-all`.
  test_stack_protector.py.
  - [x] `-fstack-protector-explicit` and the `stack_protect` /
    `no_stack_protector` attributes, both spellings (2026-11-20): gcc's
    rules -- stack_protect protects a function under any -fstack-protector*
    and is the only thing -explicit protects; no_stack_protector leaves it
    unprotected even under -all (and wins over stack_protect, with a
    -Wignored-attributes warning). SYMF_STACK_PROTECT / SYMF_NO_STACK_PROTECT
    on the function's Sym, decided at its start (`ssp_begin_function`,
    `ir->ssp_off` stops the body's choice), the same at every -O. Under a
    -fstack-protector* flag such a function is not inlined (gcc does not
    inline across a mismatch); without one the attributes change nothing.
    `__has_attribute` 1, `__SSP_EXPLICIT__ 4`. test_stack_protector.py
    (canary per mode and attribute at -O0..-Os, a QEMU overrun caught in a
    stack_protect function under every mode, objects unchanged without a flag
    and under -explicit with no attribute).
- [x] **`-b` bounds checking on ARM** (2026-11-21), the armv8m cross and
  QEMU only: the runtime's memory rules the board out.  The Makefile adds
  `-DCONFIG_TCC_BCHECK_CROSS=1` to the cross alone (`configure
  --disable-bcheck-cross` drops it), which turns `CONFIG_TCC_BCHECK` on over
  config.mak's `=0`; the device compiler (`TCC_IS_NATIVE`, its own configure)
  never has it.  Old tcc's scheme as ordinary IR calls
  (`frontend/gen/op/bcheck.c`):
  - `p + n` -> `__bound_ptr_add(p, n*size)` (inside p's region or one past
    it); a dereference of that result patches the same call's callee symbol
    (bc_patch: result vreg -> symref) to `__bound_ptr_indirN`, so the access
    is checked against p's region; any other marked access (`*p`, `s.x`,
    `q->f`) is a fresh `__bound_ptr_indirN(&lvalue, 0)`.  The access is marked
    by indir/member access (VT_MUSTBOUND) and checked by gv(), vstore, an asm
    operand at parse time, or tcc_ir_put (`bcheck_ir_operand`, for loads
    folded into an operation);
  - a local array or struct: `__bound_new_region(&x, sizeof x)` where it is
    declared, `__bound_delete_region(&x)` for every local registered so far at
    each return (the value read first) and at the end -- not in an inlined
    body, whose locals die with the caller's frame;
  - globals: the `.bounds` (address, size) table of init/alloc.c, ended at
    link time and named `__bound_globals` (tcc_add_bcheck);
  - the runtime is `lib/bcheck-bm.c` (newlib, bare metal): a sorted array of
    regions, `malloc`/`calloc`/`realloc`/`free` over `_malloc_r` with one byte
    of padding, freed blocks filled with 0x5a and kept registered in a
    64-block quarantine.  Errors print old bcheck's line (`BCHECK: 0x.. is
    outside of the region (0x..0x..)`, `...: use after free`, `BCHECK:
    freeing invalid region`) with the caller's pc, then exit(255).  The
    harness links with -nostdlib, so the tests link it by hand.
  Without -b, objects are byte-identical to the baseline compiler's (4620
  objects: tests/ir_tests/*.c at -O0..-Os against combined/2026-11-20, and
  -g on 1532).  Every numbered QEMU test (618, each at one level: a fifth at each
  of -O0..-Os, the last fifth at -O2) built with -b prints what it prints without,
  except three that -b's calls expose: two asm tests
  (docs/bugs/asm-memory-operand-base-in-clobbered-register.md) and a
  computed-goto loop (docs/bugs/computed-goto-sanitized-add-lost.md).  The
  sweep found and fixed: an asm operand checked while its operands were set
  up, a bitfield unit read past a packed struct, and statics written only
  through checked member stores folded to their initializer.
  test_bcheck.py (local, global, struct, malloc'd block, calloc, one past the
  end, underflow, inlined body, use after free, realloc's old block, double
  free; correct cases incl. VLA, string literal, struct copy, frame reuse and
  a static written through members; no `__bound` symbol or `.bounds`
  without -b).
  - [ ] Not checked yet: VLAs and `alloca` (old tcc's `__bound_new_region` from
    alloca, freed with the frame), address-taken scalars (old tcc registered
    them too), the C library's own reads and writes (old bcheck's
    `__bound_memcpy`/`strcpy`... redirections), sub-objects (`s.a[4]` inside
    `struct { int a[4]; int tail; }` is in `s`), a struct copied whole
    (pointer arithmetic only), setjmp/longjmp (registered locals of the
    frames longjmp skips stay until a new object overlaps them), and a local
    declared textually after a `return` but registered by an earlier loop
    iteration (not dropped at that return).  YasOS libc has no runtime: `-b`
    is for the newlib QEMU runs.
- [x] **`-ftrapv`**: trap on signed overflow: the `+ - *` and negation subset
  of the first item (gcc's option; `-fno-trapv`). In test_sanitize.py.

## Open items of the checks above

- [x] The older frontend warnings report once (2026-11-20): int-conversion,
  incompatible-pointer-types, discarded-qualifiers (so write-strings),
  implicit-function-declaration, return-type, implicit-int, nonnull (three
  copies at -O2 before), implicit-move, the yasc type rules and the plain
  body warnings go through `tcc_warning_once` / `tcc_warning_once_always` (no
  option): a `static inline` called twice says each once at every -O, with its
  line (`#pragma diagnostic` at the body's line), quiet in a system header.
  Two of one line and text are still two (gcc tells them apart by column):
  each body parse (`tcc_diag_parse_enter`: a function's compile, its
  check-only parse, an inline expansion, a cleanup replay) numbers them, and
  the k-th of one parse is the k-th of every other.  A warning met by the
  speculative constant evaluation of a call it folds is said by the body's
  check-only parse.  `tests/ir_tests/test_report_once.py`.  Errors
  (`tcc_error`) unchanged; file-scope and preprocessor warnings stay plain.
  Left: a void `static inline` whose every call is dropped without a parse
  (`g(); g();` with `{ return 1; }`) says nothing at -O1+ under `-ccompat`
  without -Wall, where nothing asks for the check-only parse.
- [x] One "report once" mechanism (2026-11-20): `tcc_diag_once()` in
  `source/driver/libtcc.c`, one set of hashes per unit over (kind, option or
  key, file, line, text), reset in `tcc_diag_reset`, looked at only when
  something is reported (an option that is off and no `#pragma diagnostic`
  costs one test).  `tcc_warning_once` / `tcc_warning_once_at`,
  `tcc_fe_warning` (values-dependent: also quiet in the speculative parse),
  `tcc_diag_first` / `tcc_diag_marked`, the attribute checks' `attr_warn` and
  the `-Wunused` declaration marks are wrappers over it.  One replay-file
  variable: `diag_replay_file` (`inline_replay_file`, `attr_expansion_file`
  gone); the replayed body's file decides the system-header test.  One policy:
  the check-only parse keeps a warning with its line (the set drops what an
  expansion said), so the `tcc_warning_once` checks speak for an uncalled
  static inline of the main file at every -O, as the constant checks did.
- [x] `-Wextra` (2026-11-20): an option group like -Wall (`WD_EXTRA` in
  `options_W`; other checks join it with that flag): implicit-fallthrough,
  empty-body, sign-compare, type-limits; unused-parameter and
  unused-but-set-parameter only with `-Wunused` or `-Wall` too
  (`WD_EXTRA_UNUSED`, either order), as gcc.  `-Wno-extra`, `-Werror=extra`,
  `#pragma GCC diagnostic "-Wextra"`; a member the command line names keeps
  what it says.  Not in -Wall nor the safe default.  tcc has no
  missing-field-initializers.  `tests/ir_tests/test_wextra.py`.
- [x] `SymAttr` is full (32 bits): the next 1-bit per-symbol fact is a
  `SYMF_*` flag in `SymLocalFacts.flags` (`sym_flag_set` / `sym_flag`,
  tcc.h; one bit to test when the Sym has no facts, `attr_facts` being the
  general marker), a value a field there.  No existing bit moved: the rare
  ones (nodebug, param_array, nonnull) sit on struct members and parameters,
  which are copied and spilled, and the table is keyed by the Sym's address.

- [x] At -O1+ a static function every call inlines is never compiled on its
  own: a check-only parse at the end of the unit runs the flow checks on it
  (and on an uncalled static of the main file), with the frontend's warnings
  muted (the expansions gave them). Dropped statics get the flow checks too.
- [x] `-Wlifetime` no longer turns the SSA path back on at `-O0` (43f3f18b06
  reverted 2026-11-20): the checks run on the raw IR and never needed it, so
  the safe default keeps -O0's fast path and its -O0 objects equal
  `-Wno-lifetime`'s (test_o0_lifetime_ssa.py).
- [x] Known limits of the flow checks (state per path) -- each one below
  is fixed (test_lifetime_paths.py passes, 2026-11-20):
  - [x] correlated conditions (2026-11-20, test_lifetime_paths.py): a local
    or parameter (address not taken) tested against a constant twice, or
    tested and assigned a constant, outside inlined bodies, is a predicate
    (32 at most); each block keeps up to 8 contexts, each knowing some
    predicates' values (bit masks) with its own state; a test whose value a
    context knows sends it down one edge, a constant assigned decides it,
    anything else forgets it; past 8 the closest contexts merge, keeping
    what both know.  A block reached by several contexts reports on their
    join, and leaving the function past its last jump uses the contexts that
    can.  `if (c) free(p); ... if (!c) free(p);`, a `done` flag and `if (c) p
    = malloc(...); ... if (c) free(p);` are clean.  `int ok = p != NULL; ...
    if (ok) *p` (also `!p`, `_Bool b = p`): a local whose values are all one
    null test of a pointer variable is linked to it (a state entry) while
    neither changed.  Cost: one block walk per context; at most 16384
    contexts in a function and 1 MB of states (fewer contexts per block in a
    big function), joined after 24 walks.  A synthetic 5000-branch function:
    -O0 0.03 s with or without; a 600-branch one with 40 predicates 7.5 ms
    against 4.6 ms before.  The caps can make a function with many more blocks
    at -O1+ (inlined bodies) less precise than at -O0.
  - [x] values followed within a block only (`c ? p->v : 0`): fixed
    2026-11-20, see "maybe members" above;
  - [x] a variable whose address is taken is not followed -- except when
    every address taken goes straight into an owned out-parameter
    (`getline(&line, ...)`, test_yasc_outparams.py) or a parameter that only
    reads (`T * owned const * view`, `const T * view`; an inlined callee's
    too), which reads it there (2026-11-20, test_lifetime_paths.py
    readonly_*; a pointer to a const owned pointer is no out-parameter);
    likewise a local struct or array (slot items);
  - [x] an array element at a variable index (2026-11-20, test_lifetime_index.py):
    owned elements (`char * owned a[N]`, `b[i].buf`, `int owned(close)
    fds[N]`) -- per array (one member of each element of the innermost
    array) and index variable an item 'a[i]' follows the element at i while
    i is not written; writing i, or the end of the array's scope, joins it
    into every element, which become vague (only definite states are
    reported there).  Loops over i are summarised: a loop whose only write
    of i is `i += c` at the end of every trip, reaching the array only at i,
    has a new element each trip; its trips' results go into the elements
    where it is left.  `for (i = C0; i < N; i++)` (<=, counting down, a
    declared i) with C0 and the last value inside the array, left only by
    its test, sets the elements it visits exactly: `for (...) a[i] =
    malloc(...)` then `for (...) free(a[i])` is clean; without the free loop
    "'a[i]' leaks: it may still own memory" (once per array); freed twice by
    two loops, or `a[1]` used after the free loop, is reported; `free(a[i]);
    use(a[i])` with i unchanged is a use after free.  Any other loop (a
    variable bound, a break or goto out, a `continue` to the header) joins
    its result into every element, so nothing definite comes from it.  a[i]
    and a[j] are not assumed to be the same element, and an array indexed by
    two variables never says that a[i] leaks.  Still not followed: an index
    that is not a variable (`a[i + 1]`, `a[f()]`: the local is not), an
    address of an element at a variable index taken (`&a[i]`), arrays past
    the 64 items of a function, more than 16 index items or 16 loop
    summaries (64 loops looked at) per function.  Also fixed then: a
    conditional jump past the function's end checks the items' leaks, and
    items declared in an inlined body are silent (its callee reports them),
    so the report is the same at every -O.  The yasc stress corpus (ir_tests,
    gcc torture, examples/yasc at -O0 and -O2) has no new report: it has no
    owned arrays;
  - [x] a borrow through a call's result: checked for use after free
    (2026-11-20);
  - [x] an owned out-parameter's status through a variable (`int r =
    posix_memalign(&p, ...); if (r) return;`, 2026-11-20): a local defined
    once by the call's result (address never taken) stands for it
    (test_yasc_outparams.py out_status_in_variable); assigned more than
    once, a state entry per local says which calls' results it may hold on
    each path (7 calls at most), and their writes are decided by its test
    (status_variable_reused, test_lifetime_paths.py status_*);
  - [x] a dereference of a value with no name where it may be null (a
    call's result `find(k)->v`, also `find(k)->next` and `keys()[i]`; the join
    of `?:`, `(c ? p : q)->v`): "a pointer that may be null is dereferenced",
    once per line (2026-11-20, test_lifetime_paths.py).  An inlined call's
    result (-O1+) is as maybe as the call's type says, so `T * view q =
    find(k)` is reported at every -O (it was not when find was inlined);
    only a callee whose body is a constant `return 0;` folds into a null
    load at -O1+ (-Wnull-dereference reports that one).
- [x] valgrind: an uninitialised read in `codegen.c:3762`
  (`ir_codegen_scan_interleaved_word_copy`) at -O1 and -O2, with or without
  -Wlifetime (seen 2026-11-20 while building the member tracking). Fixed
  2026-11-20: a word load not followed by its store stopped the scan with
  the run's offsets unset, and their alignment was tested before the word
  count (result thrown away, objects unchanged). The same sweep (every 6th
  `tests/ir_tests/*.c`, -O1/-O2) found a second one, fixed too: the padding
  of a string token's last word (`tok_str_add2`), compared by
  `save_function_body`. Test: `tests/ir_tests/test_valgrind_codegen.py`
  (memcheck runs; fail without the fixes).
  - [x] The spill file got uninitialised bytes of the token-symbol arena
    (seen in writes from `sym_params_release` / `tok_names_spill`, which share
    the spill's write buffer): a name's padding byte to 2 and a retired
    chunk's free tail, both written whole by `tok_names_spill`. Fixed
    2026-11-20 (`toksym_name_copy` / `tok_name_load` write the padding,
    `toksym_bump` clears the tail it retires); objects unchanged.
    test_valgrind_codegen.py spill_file_gets_no_uninitialised_bytes (1500
    prototypes, -O0/-O2; failed before).

## Next: the order agreed on 2026-11-20

1. ~~C23 `auto` / `__auto_type`~~ -- done 2026-11-20.
2. ~~Explicit `move(p)` / `borrow(p)`~~ -- done 2026-11-20.
3. ~~`view` does not escape, `borrows(...)`~~ -- done 2026-11-20.
4. ~~`owned(dealloc)` and `takes(p)`~~ -- done 2026-11-20 (libc: realloc,
   fdopen, strchr and friends).
5. ~~`owned(close)` file-descriptor handles~~ -- done 2026-11-20.

Done after that (2026-11-20): owned out-parameters (test_yasc_outparams.py:
`&v` passed to a parameter pointing at an owned pointer keeps v followed;
in/out and out; a status result's test decides) -- libc getline, getdelim,
posix_memalign, getaddrinfo; borrows(member) killed when its source member
changes; locals before a borrows(member) store; struct-level borrows.
Done after that (2026-11-20, later still):
- owned items of local structs and arrays (`b.buf`, `arr[0]`, `fds[1]`),
  followed by frame offset (tcc_ir_slot_check, test_yasc_slots.py): stores,
  reads, arguments, moves, null and handle tests, the local's scope end,
  its address to an owned out-parameter (`pipe(p)`); a local whose address
  goes anywhere else (but a pointer that cannot write) or that is indexed
  with a value that is neither a constant nor a variable is not followed
  (a variable index: see "Known limits" above);
- objects owned as a whole, `regex_t owned(regfree) re` (test_yasc_objects.py):
  `out T owned(release) * view` writes one, `drop` releases it with its
  deallocator checked, a read-only pointer uses it; written into a local not
  declared owned is reported.
Done later still (2026-11-20, feat/p1-flow): values across blocks, maybe
members of local/global structs and global pointers, a local linked to the
member it copies, the use-after-free check through a call's borrowed
result, a status read through a variable (`int r = f(&p); if (r)`),
`nonnull` as not maybe.
Done later still (2026-11-20, feat/rest-flow): correlated conditions,
array elements at a constant index as member bases, values with no name
dereferenced, read-only address arguments, status locals assigned twice.

## P3: later

- [ ] **Thread-safety annotations** (clang `-Wthread-safety`: `guarded_by`,
  `requires`, `acquire`/`release`) for pthread code and maybe the kernel's C side.
  Deferred (decided 2026-11-20): YasOS has no threads yet, so there is nothing
  to check; pick it up when threads land. Design: attributes `capability("mutex")` /
  `lockable` on a type, `guarded_by(m)` / `pt_guarded_by(m)` on a global or
  member, `requires_capability(m)` / `exclusive_locks_required(m)`,
  `acquire_capability(m)` / `release_capability(m)` on functions,
  `no_thread_safety_analysis`; kept in SymLocalFacts (SymAttr is full), the
  lock named by its token (a global, or a parameter by index). A frontend
  hook per call (as tcc_ir_taint_note_call) records which lock it takes or
  releases; `source/ir/threadsafety.c` runs on the raw IR like ir/taint.c:
  a bit per lock "held", entry state from requires_capability, a join ANDs
  (held on every path); a load or store of a guarded global (its symbol in
  the operand) or member (the frontend notes the temp of its address, as
  lifetime's members) without its lock reports "reading 'x' requires holding
  'm'"; release when not held, and held at a return (not released by an
  acquire function), report too. Limits to document: locks by name only (no
  aliasing, no lock arrays), no try-lock edges at first, guarded members
  through one base only. Off by default (`-Wthread-safety`).
- [x] **Taint for `-Wformat-security`**: a format from `argv`, `getenv` or `read`.
  Done 2026-11-20 as `-Wtainted-format` (`source/ir/taint.c`; off by default,
  not in -Wall: gcc has no such check). Raw IR before any pass: a bit per
  variable (and one for the frame's arrays together) "holds or points to
  untrusted bytes", forward over the CFG; temps for the whole function, with
  the variable they point into. Sources: main's argv, getenv/secure_getenv,
  the buffer fgets, gets, fread, read, pread, recv, recvfrom, getline,
  getdelim fill, and the new attributes `__attribute__((tainted_result))`
  and `tainted_arg(n[, m...])` (what argument n points to, from 1, after the
  call). Sinks: the format of a `format(printf, n, m)` function, or the
  printf family by name. "the format of 'printf' comes from untrusted
  input: write printf("%s", ...)". Calls inside an inline expansion are
  neither (same result at every -O). Nothing is kept for a unit with no
  source. Tests `test_tainted_format.py`.
  - [x] Globals, struct members, summaries (2026-11-21): memory by object --
    each local array/struct of the frame (its extent from `sym_push`,
    `tcc_ir_taint_add_local`), each global, what a pointer variable points
    to -- with a whole fact plus member paths at constant offsets
    (lifetime.c's base + offset): a store replaces its path's fact
    (`s.fmt = getenv(...)` taints s.fmt, not s.name; `s.fmt = "ok"` cleans
    it), a fill taints from its offset on, an index added reaches every
    path from its lower bound on.  A pointer variable assigned only
    addresses into one object is that object's alias (`p = buf;
    fgets(p, ...); printf(buf)`).  The facts are masks (untrusted, and
    "parameter j's", j < 7), so one walk reports and gives the summary:
    which parameters (and sources) reach the result, what pointer
    parameters point to (by member) and which globals get written after the
    call, which parameters reach a format.  Functions are kept in a compact
    form to the end of the unit (`tcc_ir_taint_unit_end`, 16 MB at most;
    past it a function is checked at once with the summaries known), the
    summaries iterated to a fixpoint -- callees defined later count -- then
    every function is walked once to report.  A format reached in a helper
    is reported at the sink, with a note at the call ("untrusted input
    reaches it through the call to 'say' in 'main'"), once per sink.  An
    inline expansion is the call it replaces (calls inside it stay neither
    sources nor sinks; the expanded function's summary is applied where its
    body ends), so -O0..-Os say the same.  memcpy/strcpy/strdup & co carry
    what they copy.  Bounded: 256 memory facts per function, 16 summary
    entries, no walk whose state passes 32 MB.  Tests
    `test_tainted_format.py` (globals, members, a helper returning
    getenv(), a helper filling a buffer with read(), both defined after
    main, a sink two calls down, recursion, pragma at the sink).
    - [ ] Not followed: other units (summaries are per unit), memory
      through pointers of unknown origin, indirect calls, scanf `%s`
      buffers, a large struct copy made by a helper call (`s = *p`), what a
      summary writes through a parameter past the seventh.
- [x] **Machine-readable diagnostics** (`-fdiagnostics-format=json` or SARIF)
  for CI annotations (2026-11-20, `driver/diag_out.c`, #included by
  libtcc.c). `json`: gcc's array of {kind, message, option, locations,
  children, fixits}, notes as children; `sarif`: SARIF 2.1.0, one run, ruleId
  "-Wname" (or "error"/"warning" when no option names it), level, message,
  physicalLocation uri/startLine, notes as relatedLocations, fix-its as fixes.
  On stderr, each result written once no note can join it; one document per
  process (`tcc -c a.c b.c`), closed at exit, also after an error stops the
  compile. Output to `tcc_set_error_func()` stays text. `-fmax-errors=N`
  (error recovery's) ends the document the same way, its "compilation
  terminated" line a note of the last error. No columns (tcc keeps none).
  Tests `test_diag_format.py`.
- [x] **Fix-it hints** (`note: use snprintf`) for deprecated calls and yasc
  keyword renames (2026-11-20). `tcc_fixit(line, word, where, text)` after a
  diagnostic: the word is found once on its line by reading the file back
  (no columns in tcc), else no fix-it, only the note; `tcc_fixit_next()` for
  an error, which aborts first. Shown with `-fdiagnostics-parseable-fixits`
  (gcc's `fix-it:"f.c":{l:c-l:c}:"text"`) and in the JSON/SARIF output.
  Given: a `deprecated` libc function with a known replacement (sprintf,
  vsprintf, strcpy, strcat, gets, tmpnam, tempnam, mktemp, strtok, atoi,
  atol, asctime, ctime, getwd, bzero, bcopy, usleep: a note;
  index/rindex: a rename fix-it) -- effective once libc marks them; "write
  move(p)" (fix-its around p); a plain pointer parameter (note, fix-it
  `* view `). Tests `test_diag_format.py`.
  - [x] `sprintf(buf, ...)` -> `snprintf(buf, sizeof buf, ...)` (2026-11-20),
    also `vsprintf` -> `vsnprintf`, `strcpy(d, s)` -> `strlcpy(d, s, sizeof
    d)`, `strcat` -> `strlcat`: only where the destination's size is exact
    (`builtin/objsize.c`: a declared array, a member, a constant allocation,
    at a known offset); `sizeof buf` when the destination is written as a
    char array's name, else the constant (`snprintf(p, 64, ...)`,
    `strlcat(buf + 2, s, 14)`).  The note "use snprintf(buf, sizeof buf,
    ...)" and two fix-its (the new name; `, size` after the first argument,
    after the second for strlcpy/strlcat) go with the first diagnostic of the
    call: the deprecated warning (once libc marks them; such a call's report
    now waits for its arguments, so it comes after -Wformat's of the same
    call), else -Wformat-overflow / -Wstringop-overflow.  The call's name and
    the arguments up to that one must be on the call's line, read back from
    the file (`tcc_fixit_call`); else only the note.  A size not known: the
    general note, as before.  Found with it: `__builtin_object_size(buf + 2,
    0)` of a global array said 16 (the offset folded into the symbol, the
    record missed it): 14 now -- the -Wstringop-overflow sizes too.  Tests
    `test_fixits.py`.
  - [x] owned vs view decided from the body (2026-11-20,
    `decl/plain_param.c`): a definition's plain pointer parameter is reported
    at the end of its body (the parameter's line, still an error, after the
    body's own diagnostics and before its flow checks; such a definition is
    never deferred, so the order is the same at every -O): `* owned` when
    the body frees it (free, realloc, a deallocator, a `drop` parameter),
    passes it to an owned parameter, stores it where it outlives the call or
    returns it as owned; else `* view`; `maybe` too when it is tested for null
    or passed to a borrowing maybe parameter.  The note says why and where
    ("the function frees 's' (line 8): write '* owned'"); the fix-it goes
    after the '*', or before the name when the line has two.  A prototype, a
    nested function, a function pointer's parameter: `* view` at once, as
    before.  Tests `test_fixits.py`.
    - [x] Limits lifted (2026-11-21): an alias is followed -- a local given
      a followed parameter's or local's value (`q = p`, `char *q = p`, also
      an annotated or `auto` one) carries its uses to it until it is given
      something else ("the function frees 's' through 'q' (line 9)"); in
      source order, not per path.  The error names its pointer ("... say
      owned or view for 's'"), so two plain parameters (or locals) on one
      line give both errors and both notes; the fix-it goes after the '*',
      else rewrites `*s` (`* view s`, found once with an identifier boundary
      -- `tcc_fixit_next_alt` takes two alternatives now), else before the
      name.  Tests `test_fixits.py`.
  - [x] A plain pointer that is not a parameter (2026-11-21): a local's own
    pointer (an automatic variable's outermost '*' in its declarator) waits
    for the end of its body like a parameter's and is reported with it, in
    line order: `* owned` when it is given an owned value (`char *p =
    malloc(n)`), freed, passed to an owned parameter, stored into an owned
    pointer or returned as owned, else `* view`; `maybe` also when it is
    tested for null, passed to a maybe parameter or given null or a maybe
    value ("the function gives 'p' an owned value that may be null (line
    12): write '* owned maybe'").  Inner levels (`char **pp`'s first '*'),
    statics, typedefs and nested declarators are said at once, without a
    fix-it, as before; in an inline expansion the function's own body says
    it.  The reports move with the function's other diagnostics when -O1+
    compiles it at the end of the unit (the same set at every -O).  Tests
    `test_fixits.py` (each decision, the fix-its applied and compiled clean
    with -Wall after -Wimplicit-move's own fix-its), `test_max_errors.py`,
    `test_diag_format.py`.
- [x] **Cross-TU checks**: the same symbol declared with different types in
  two units, or a header that does not match its definition. The linker has
  every symbol, but types would need debug info. Done 2026-11-20 as
  `-Wcross-tu` (off by default, not in -Wall): each unit records its global
  symbols' types as normalised C (`frontend/gen/decl/typesig.c`: no storage
  class or yasc qualifiers, parameters' own qualifiers dropped and arrays
  decayed, enums as their integer type; `int f()` and `int a[]` stay open)
  -- with -c in a non-allocated `.tcc.typesig` section, only with the flag
  (objects without it are byte-identical), kept by `tcc -ar`, merged by `-r`,
  never in a linked output. The link with `-Wcross-tu` compares them
  (`obj/tccelf_load.c`): "'f' declared as 'int (int)' in a.c, defined as
  'long (int)' in b.c" (also two declarations, two definitions); one report
  per name; `-Werror=cross-tu` fails the link. Tests `test_cross_tu.py`.
  - [x] Locations and struct layouts (2026-11-21): a record carries where
    (`D`/`U`/`S` + "file:line": a definition, an extern declaration, a
    file-scope struct/union body; `tcc_typesig_note()` from decl.c and
    struct.c, only with the flag), and the report says it ("'f' declared as
    'int (int)' at api.h:1 in a.c, defined as 'long (int)' at b.c:2").  Every
    complete named struct/union a global's type reaches gets an `S` record
    with its layout (size/align, members' names, types, offsets, bit-field
    widths); the link compares the layouts behind every pair of compatible
    records of one name, recursively (depth 16, cycle guard), names the
    first member that differs, once per tag, and says nothing more of an
    outer struct that moved only because a member's struct changed.
    Anonymous structs are written out in the signature.  Objects without
    the flag stay byte-identical.  Tests `test_cross_tu.py`.
    - [ ] Limits: units are told apart by file name (two units of one name
      share their layouts); not checked by `-run`, which this cross has no
      runtime for (it parses the option and does nothing with
      TCC_OUTPUT_MEMORY).
