# C headers written from yasc headers (`tcc -yasc-c-header`)

A library's header is written once, in yasc. `tcc -yasc-c-header yasc/x.h -o x.h`
writes the regular C header that gcc, clang and tcc's C units read. Nobody edits
the C header; yasos libc commits both and checks them in CI (`make headers` /
`make headers-check` in libs/libc, where both live in `include/`: the sources
in `include/yasc/`, the C headers beside them).

Why: before, each libc function with a yasc contract was declared twice, once in the
C header (GCC attributes) and once in `sys/yasc/<h>.h`, and nothing compared the
two. They drifted: `<dirent.h>` said `opendir` is released by `closedir`, the yasc
twin said `free`, so every yasc unit that included it failed with a redeclaration
error.

## Layout

- `yasc/x.h`: the source. It starts with `#pragma yasc`, so every file that
  includes it reads it as yasc. A tree with headers in subdirectories mirrors them:
  `yasc/sys/socket.h` gives `sys/socket.h`.
- `x.h`: written by the tool. A yasc unit (`__YASC__`) includes the source, so it
  gets the yasc declarations themselves. Everything else gets the translation:

```c
/* Written by `tcc -yasc-c-header` from yasc/x.h: edit that file, not this one. ... */
#ifdef __YASC__
#include "yasc/x.h"          /* "../yasc/sys/x.h" from sys/: found next to it, whatever the -I */
#else
... the translation ...
#endif /* __YASC__ */
```

The source's leading comment block (the license) stays above the banner. The tool
writes the file only when its bytes change, so it never touches a header's mtime
for nothing.

## What the source may contain

| In the yasc header | In the C header |
|---|---|
| `#pragma yasc` | left out |
| `#pragma tcc c_begin` ... `#pragma tcc c_end`: a regular-C section, for declarations nobody annotated yet (a plain pointer is an error in yasc) | the section as written, without the two lines |
| `#ifdef __YASC__` ... `#else` ... `#endif` (also `#if defined(__YASC__)`, `#ifndef __YASC__`): what only yasc can say (slices, yasc inline functions) | the `#else` part only |
| other preprocessor lines | as written; in a `#define` body the yasc words are left out and `nullptr` becomes `((void *)0)` |
| `owned`, `owned(D)`, `view`, `maybe`, `drop`, `borrows(...)`, `takes(...)`, `in`/`out`/`inout`, `[maybe]` / `[owned]` in a parameter's brackets | left out (`T * view s` reads `T *s`) |
| a parameter `T p[maybe :0]` (NUL inside the buffer) | `T p[]` and `__yl_cstr(N)` |
| a result `T * owned(D) ...` (`owned` alone: `free`) | `__yl_dealloc(D, N)`, N: D's `drop` parameter when D is declared above in the same header, else 1 |
| `c_api(T)` after a `*` (docs/yasc_c_api.md) | T in place of what is left of it; on a prototype also `__yl_c_api(n, yasc type)`, n 0 the result, else parameter n |
| a struct member `T * view m` / `T * view maybe m` | `T *m __yl_view` / `__yl_view_maybe` |
| attributes written on a declaration (`__yl_nonnull(1)`, `__yl_alloc_size(2)`, `__yl_malloc`, ...) | as written, before the derived ones |

The derived attributes are `<sys/_attrs.h>` macros, so gcc, clang and older tcc
get what each of them understands. If the source does not include that header,
the tool adds `#include <sys/_attrs.h>`.

## What is not derived: `nonnull`, `returns_nonnull`

A `view` parameter without `maybe` tells a yasc caller "do not pass null". A C
`nonnull` tells the compiler that a null argument is undefined, and gcc deletes
the callee's null checks because of it. The two differ where the function handles
null on purpose. `getline(NULL, &n, f)` and `posix_memalign(NULL, ...)` are defined
to fail with EINVAL, so their C declarations must not say `nonnull` even though
their yasc parameters are `view`. The same holds for `returns_nonnull`
(`asctime` may return null on overflow). So the source writes `__yl_nonnull(...)` /
`__yl_returns_nonnull` itself, where null really is undefined, just as the C
header did before.

## Errors (exit 1, nothing written)

- a yasc slice (`[]T`, `[:0]T`) outside `#ifdef __YASC__`: C has no form for it;
- a yasc statement (`defer`, `try`, `unsafe`, ...) in an inline function's body
  outside `#ifdef __YASC__`;
- a preprocessor line inside a declaration that has yasc words (split it);
- `#elif` right after a `__YASC__` condition (write `#else`);
- a declaration without its `;`, a `c_begin` without its `c_end`, a conditional
  without its `#endif`;
- an input that is not under a directory named `yasc` (the C header includes the
  source by that path).

## The dialect sections across `#include`

`#pragma tcc c_begin` / `c_end` set the dialect of the rest of the file (the file's
own flag, as `#pragma yasc` does), so an `#include` inside a C section returns to
C. The pragma also takes effect when the parser reads it in the lookahead past a
saved body, e.g. right after a header full of `static inline` functions. Before
2026-11-26, both cases fell back to yasc, so the section's plain pointers became
errors (`test_yasc_c_header.py::test_c_section_survives_an_include`).

Tests: `tests/ir_tests/test_yasc_c_header.py`. The tool is `source/driver/yasc_cheader.c`.
