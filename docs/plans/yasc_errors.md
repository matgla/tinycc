# yasc: defer, errdefer and error unions

Zig-style error handling for the yasc dialect (`-std=yasc` or `#pragma yasc`),
without exceptions: no unwind tables, no personality routine. Every
construct here is lowered in the frontend into branches, so the IR, the
register allocator and `-O0` speed are unchanged. Outside the dialect every
keyword below is an ordinary identifier.

Decided 2026-11-20: integer error sets compatible with errno, conversions done
automatically where they are safe, and a small reflection layer
(`error_name`).

## Stage 1: `defer` (done)

```c
defer note("done");          /* one expression statement */
defer { unlock(m); free(p); } /* or a block */
```

- The statement runs whenever its scope is left: falling off the end,
  `return`, `break`, `continue`, `goto` out. Last registered runs first,
  in the same list as `__attribute__((cleanup))` variables.
- A defer not reached yet does not run. A backward `goto` over a defer runs
  it, like leaving and re-entering the scope.
- The body is saved as tokens and compiled again at every exit
  (`source/frontend/gen/stmt/defer.c`), the way Zig expands defers. No
  run-time cost beyond the body's own code, but it is duplicated per exit.
- Names in the body bind where the defer is written: locals declared later
  are hidden during the replay.
- `return EXPR;` computes EXPR before the defers run (an lvalue is loaded,
  flags are settled, a struct is copied).
- Not allowed in a body: `return`, `break`/`continue` out of it, `goto`,
  labels, `case`/`default`. A defer directly in a switch body is an error
  (a later case would jump past it); put the case in braces.
- A body's warnings are reported once, not once per exit.

## Stage 2: error sets, error unions, `try`, `catch`, `errdefer` (done)

### Error values

An error is a non-zero `int`. 0 means "no error".

- `error.NAME` where NAME is an errno macro (`error.ENOENT` after
  `#include <errno.h>`) has the errno value. The macro has already expanded
  to its number when the parser sees it, so this also works in saved bodies.
  A NAME that looks like an errno name (`E[A-Z0-9]+`) without the macro is
  an error: it would get a different value from files that include errno.h.
- Any other NAME has `0x100 + fnv1a(NAME) % 0xff00`: the same in every file
  without coordination, outside errno's range, one `movw`. As in Zig, an
  error is identified by its name: `error.BadDigit` is the same value in
  every set.
- An explicit value in a set (`FileNotFound = ENOENT`) wins in that file.
  Collisions (two names with one value, one name with two values) are
  found at link time in stage 3.

### Types

```c
error FsError { FileNotFound = ENOENT, EACCES, BadPath };  /* a set */
error FsError e = error.BadPath;   /* a variable of the set */
error any = e;                     /* `error` alone: any error ("anyerror") */
!int parse(const char *s);         /* an error union: an int or an error */
```

- An error set is an `int` enum whose members are not global names; its
  members may be errno numbers (`EACCES` expands to one).
- Conversions: a set or `error` converts to `int` implicitly (errno
  compatible: `errno = e;`, `return -e;`). A set converts to `error` and to
  a set holding all its members. `int` -> error only with `error_from(x)`.
  `error.X` converts to a set only if X is a member.
- `!T`: T is `void`, an integer type of at most 32 bits, a pointer, an enum
  or an error set (bigger payloads: later, with a hidden pointer). ABI: a
  64-bit return in r0:r1, the value in r0, the error in r1. Plain C sees an
  `unsigned long long`. Locals and parameters of type `!T` are allowed.
- An error union can only be used by `try`, `catch`, `return`, assignment
  and initialization of the same type, and passing as an argument of that
  type. Arithmetic, comparisons, casts, tests and discarding it are errors.

### Statements and operators

```c
int v = try parse(s);                 /* error: run errdefers, defers, return it */
int w = parse(s) catch 0;             /* a fallback value */
int x = parse(s) catch |e| fallback(e); /* the error, then a fallback value */
parse(s) catch |e| { log(e); return e; } /* a block that must not complete */
errdefer free(buf);                   /* runs only when the function returns an error */
```

- `try EXPR` is a unary operator. Only in a function returning `!T`.
- `catch` binds looser than `?:` and tighter than assignment.
  `catch |e| { ... }` gives the payload only when the block cannot complete
  (its last statement returns, breaks, continues, jumps or calls a noreturn
  function, or all arms of its last `if` do; loops count as completing).
  A block that completes makes the whole expression `void`: fine as a
  statement, an error where the value is used.
- In a function returning `!T`: `return error.X;`, `return e;` (a set or
  `error`) and a failing `try` are error exits; `return value;` is a normal
  exit; `return eu;` (an error union) tests at run time and takes one of the
  two.
- `errdefer` runs only on error exits. It is an error in a function that
  does not return `!T`, and in a defer body. One that no error exit runs is
  still compiled once without code, for its diagnostics.
- Lowering (`source/frontend/gen/expr/yasc_errors.c`): a `try` is one
  `cbnz` on r1 after the call; an error passes through in r0:r1 untouched.
- `error.NAME` is read without macro expansion (`pp_error_name`), so the
  name survives for messages; in saved bodies (`-O1`+, defers) it is already
  a number, which gives the same value.

## Stage 3: reflection, libc glue, link checks (done)

- `error_name(e)` (a builtin) calls libtcc1's `__yasc_error_name`
  (`lib/yasc.c`), which scans `__start_yasc_errname`..`__stop_`. Unknown
  values give `"(unknown error)"`, 0 gives `"(no error)"`; for an alias the
  errno name wins (`FileNotFound = ENOENT` prints `ENOENT`). `strerror(e)`
  still gives the message of an errno value.
- Records: at the end of a file (`yasc_errors_emit`), one
  `{u16 value, u8 flags, u8 len, char name[len], NUL}` padded to 4 per error
  name the file used (flags: 0 explicit, 1 hashed, 2 errno). A file that
  calls `error_name` also records every errno macro its errno.h defines, so
  an errno from `error_from(errno)` has a name even if no file spells it.
  Plain C files emit nothing. The records survive `--gc-sections`.
- The linker (`tcc_yasc_check_error_names`, `source/obj/tccelf_reloc.c`)
  reads the same records: one name with two values, or two names with one
  value where one of them is hashed, is an error. Explicit aliases are fine.
- `<yasc.h>` (tcc's `include/`): `try_errno(expr)` (negative: return
  `errno`), `try_null(expr)` (NULL: return `errno`), `try_neg(expr)`
  (negative: return `-result`, for syscalls).
- Not tested yet: the records in a YAFF executable on the device (the QEMU
  tests link ELF).

## Later

- Payloads bigger than 4 bytes (hidden pointer, error in r0).
- Error sets in the union type (`FsError!int`), and `switch` exhaustiveness
  over a set.
- The `-Wlifetime` flow checks (step B) see defers and errdefers already
  expanded, so `owned` pointers freed by an errdefer need nothing special.
