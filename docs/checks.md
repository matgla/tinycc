# Catching bugs with tinycc

A guide for C programmers on YasOS to the checks YasOS's tcc runs, how to
turn them on and off, and what each one prints. Every diagnostic quoted
here is real output of `armv8m-tcc`. The examples that go with it build and
run: [`examples/checks/`](../examples/checks/README.md) has the plain-C ones
and [`examples/yasc/`](../examples/yasc/README.md) the yasc tutorial.

The list of checks, their open items and what is planned next is in
[`static_analysis_todo.md`](static_analysis_todo.md); the full design notes
of each finished check are in that file's git history (it says where).

Contents:

1. [The safe default, and `-ccompat`](#1-the-safe-default-and--ccompat)
2. [Warning options and groups](#2-warning-options-and-groups)
3. [Frontend checks](#3-frontend-checks)
4. [Buffer overflows and object sizes](#4-buffer-overflows-and-object-sizes)
5. [Flow checks](#5-flow-checks)
6. [Across units: `-Wcross-tu`](#6-across-units--wcross-tu)
7. [Many errors in one run](#7-many-errors-in-one-run)
8. [Output for tools: JSON, SARIF, fix-its](#8-output-for-tools-json-sarif-fix-its)
9. [Stack budgets](#9-stack-budgets)
10. [Run-time checks](#10-run-time-checks)
11. [Still being extended](#11-still-being-extended)

Quick reference:

| To... | write |
|---|---|
| compile old C as it is | `tcc -ccompat f.c` |
| get most of the safe default's checks on C code (section 1) | `tcc -ccompat -Wall -Wextra -Wuninitialized -Wmaybe-uninitialized -Wdangling-pointer -Wnull-dereference -Wformat-security` |
| check without building | `tcc -fsyntax-only f.c` |
| turn one warning off / into an error / back into a warning | `-Wno-shadow`, `-Werror=format`, `-Wno-error=vla` |
| silence a warning for a few lines | `#pragma GCC diagnostic push` / `ignored "-Wx"` / `pop` |
| keep the process stack small | `-Wframe-larger-than=1024 -Wstack-usage=1024`, `-fcallgraph-info=su` + `scripts/stack_usage.py` |
| catch undefined behaviour while running | `-fsanitize=undefined` (traps), `-fsanitize-recover=undefined` (reports) |
| feed CI annotations | `-fdiagnostics-format=sarif` |
| plain one-line diagnostics on a terminal too | `-fdiagnostics-plain-output` |

## 1. The safe default, and `-ccompat`

Without options, tcc compiles **yasc**: C23 plus ownership qualifiers
(`owned`, `view`, `?`, ...) for the main file, with `-Wall`, the safe
default's extra checks (the table in section 2) and the unsafe C errors on.
Headers stay regular C unless they say `#pragma yasc`, so libc's headers
keep working. The yasc tutorial explains the dialect.

A plain C file fails there, because every pointer has to say what it is and
an undeclared function is an error:

```c
#include <stdio.h>

int main(int argc, char **argv)
{
  printf("%s\n", argv[0]);
  return count(argc);
}
```

```
$ tcc -c plain.c
plain.c:6: error: implicit declaration of function 'count'
plain.c:3: error: plain pointer in yasc: say owned or view for 'argv'
plain.c:3: note: the function only borrows 'argv': write '*view'
```

**`-ccompat`** compiles regular C, as tcc always did: no yasc, no extra
checks, and the six unsafe-C errors are warnings again.

```
$ tcc -ccompat -c plain.c
plain.c:6: warning: implicit declaration of function 'count'
```

An explicit `-std=c11`, `-std=c17`, `-std=c23` (or `gnu*`) also means
regular C. `-std=yasc` asks for the dialect by name. Under `-ccompat`, a file
can still opt in with `#pragma yasc` at its top, and `-iyasc DIR` marks the
headers found in DIR as yasc.

Every check below except `-Wlifetime` works on plain C. Under `-ccompat`
you name the ones you want. A good everyday set:

```sh
tcc -ccompat -Wall -Wextra -Wuninitialized -Wmaybe-uninitialized \
    -Wdangling-pointer -Wnull-dereference -Wformat-security -c f.c
```

That is the safe default's checks without `-Wvla`, `-Walloca`,
`-Wstrict-prototypes` and `-Wignored-attributes`, with `-Wextra`'s
sign-compare and type-limits added; the six unsafe-C checks stay warnings
unless you add `-Werror=implicit-function-declaration` and so on.

[`examples/checks/01_find_the_bugs.c`](../examples/checks/01_find_the_bugs.c)
is a file with twenty bugs that this command finds.

## 2. Warning options and groups

Each check is a `-W<name>` option, written as gcc writes it:

| Option | Effect |
|---|---|
| `-Wname` / `-Wno-name` | turn one check on / off |
| `-Werror` | every warning is an error |
| `-Werror=name` | this one is an error (and on) |
| `-Wno-error=name` | this one stays a warning, even under `-Werror` or the safe default |
| `-w` | no warnings at all |
| `-Wall`, `-Wextra`, `-Wunused` | groups, below |

An option given on the command line keeps what it says, whichever group or
default would set it otherwise: `tcc -Wno-vla f.c` turns off one of the
safe default's checks and leaves the others on.

**On by default, even with `-ccompat`** (as in gcc): implicit-function-declaration,
discarded-qualifiers, int-conversion, incompatible-pointer-types,
return-type, implicit-int, unused-result, deprecated-declarations; in yasc
also implicit-move ("write move(p)").

**`-Wall`**, as gcc's C `-Wall`: the eight above, plus
return-local-addr, nonnull, address-of-packed-member, format,
shift-count-overflow, shift-count-negative, shift-overflow, div-by-zero,
overflow, tautological-compare, array-bounds, sizeof-pointer-memaccess,
sizeof-array-argument, parentheses, logical-not-parentheses, dangling-else,
switch, unused-variable, unused-const-variable, unused-function,
unused-but-set-variable, unused-value, unused-label, unused-local-typedefs,
stringop-overflow, format-overflow, format-truncation,
alloc-size-larger-than, free-nonheap-object, mismatched-dealloc.

**`-Wextra`**: sign-compare, type-limits, implicit-fallthrough, empty-body,
override-init, and with `-Wall` or `-Wunused` also unused-parameter and
unused-but-set-parameter.

**`-Wunused`**: the unused-* checks of `-Wall`.

**The safe default** (no `-ccompat`) is `-Wall` plus:

| Check | What | |
|---|---|---|
| `lifetime` | yasc ownership, `?`, `out`, borrows (section 5) | warning |
| `uninitialized`, `maybe-uninitialized` | a variable read before it is set: yasc enforces initialization (section 5) | **error** |
| `dangling-pointer` | a local's address that outlives the function | warning |
| `null-dereference` | a load or store the optimizer proves is at address 0 | warning |
| `format-security` | `printf(s)` with a format that is not a literal | warning |
| `implicit-fallthrough`, `empty-body` | the two `-Wextra` checks with few false alarms | warning |
| `strict-prototypes` | an old-style (K&R) definition | warning |
| `vla`, `alloca` | stack use nothing bounds | warning |
| `ignored-attributes` | an attribute tcc ignores although it carries a check | warning |
| `sign-compare`, `type-limits`, `unused-parameter`, `unused-but-set-parameter` | the rest of `-Wextra` | warning |
| `conversion`, `sign-conversion` | an implicit conversion that may change the value or its sign | warning |
| `shadow` | a declaration hiding another of the same name | warning |
| `missing-prototypes` | a global function defined with no declaration before it | warning |
| `override-init` | an initializer element replacing an earlier one (a union's other member) | warning |
| `pointer-arith`, `cast-qual`, `cast-align` | `void *` arithmetic, a cast dropping `const`, a cast to a stricter alignment | warning |
| `write-strings` | a string literal is `const char[]`: `char *p = "x"` is a discarded-qualifiers error | warning |
| `alloc-mul-overflow` | an allocation size that is a multiplication which may overflow | warning |
| `tainted-format` | a format string from untrusted input (section 5) | warning |
| `cross-tu` | a global declared differently in two units, found at the link (section 6) | warning |
| `unsupported` | an attribute or option tcc ignores | warning |
| `zero-as-null-pointer-constant` | yasc code spelling the null pointer `NULL` or `0`, not `nullptr` | warning |
| implicit-function-declaration, discarded-qualifiers, int-conversion, incompatible-pointer-types, return-type, implicit-int | unsafe C | **error** |
| `format-n` | `%n` in a literal printf format | **error** |

Every check tcc has is on there, except those that need a value or are not
a check: frame-larger-than=, stack-usage= (they take a limit) and
system-headers (the warnings of system headers too).  `-pedantic` is
accepted and does nothing.  `-Wno-name` turns one off.  The safe default
also checks calls against the access and null_terminated_string_arg
attributes (section 4, fortify); `-fno-fortify` turns that off.

### Changing warnings for part of a file

```c
#include <vendor.h>

int f(int a, int b)
{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wparentheses"
  if (a = b)
    a++;
#pragma GCC diagnostic pop
  if (b = a)
    b++;
  return vendor_twice(a) + VENDOR_ZERO(b);
}
```

The first `if (a = b)` is quiet, the second (line 10) warns again:

```
$ tcc -ccompat -Wall -isystem sys -c prag.c
prag.c:10: warning: suggest parentheses around assignment used as truth value
```

`ignored`, `warning` and `error` take any `-W` option, and `"-Wall"` switches
the whole group. `GCC`, `clang` and `tcc` are all accepted, and so is
`_Pragma("GCC diagnostic ...")` inside a macro. `warning` beats `-Werror`.
A diagnostic reported after its line was parsed (the flow checks run at the
end of a function) still obeys the pragmas in force at its own line. An
unknown option is ignored; `-Wunsupported` reports it.

### System headers

Warnings are quiet in system headers (found through `-isystem`, the default
include path, or after `#pragma GCC system_header`) and in the expansion of
a macro a system header defines, as with gcc. Errors are always reported.
`-Wsystem-headers` brings the warnings back. With `sys/vendor.h`

```c
static inline int vendor_twice(int x) { int unused; return x + x; }
#define VENDOR_ZERO(x) ((x) / 0)
```

included by `prag.c` above:

```
$ tcc -ccompat -Wall -isystem sys -Wsystem-headers -c prag.c
prag.c:10: warning: suggest parentheses around assignment used as truth value
prag.c:12: warning: division by zero
sys/vendor.h:1: warning: unused variable 'unused'
```

Line 12 is `VENDOR_ZERO(b)`, a macro of `sys/vendor.h`: without
`-Wsystem-headers` its expansion is quiet too.

### Same answers at every `-O`

The checks run on the source and on the IR before any optimisation, so
`-O0` and `-O2` report the same things, once each, also for a `static
inline` function called many times. There are two exceptions: `-Wnull-dereference`
looks at the optimised code, and the stack sizes of section 9 are those of
the code generated at that level. No check changes the generated code.

## 3. Frontend checks

These are decided while the source is parsed, and cost next to nothing.

### Formats: `-Wformat`, `-Wformat-security`, `-Wformat-n`

```c
#include <stdio.h>
void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void report(const char *user, long n, unsigned u)
{
  log_msg("%d bytes\n", n);
  log_msg("%s %s\n", user);
  log_msg(user);
  log_msg("%u%n\n", u, (int *)0);
}
```

```
$ tcc -ccompat -Wall -Wformat-security -Wformat-n -c fmt.c
fmt.c:6: warning: format '%d' expects argument of type 'int', but argument 2 has type 'long'
fmt.c:7: warning: format '%s' expects a matching 'char *' argument
fmt.c:8: warning: format not a string literal and no format arguments
fmt.c:9: warning: '%n' in format writes through its argument
```

Formats are checked for every function that says
`__attribute__((format(printf|scanf|strftime|strfmon, n, m)))` (also
`[[gnu::format(...)]]`), so your own logging functions are covered as soon
as they carry the attribute. A C library's `printf` is checked when its
header carries it too: newlib's and glibc's do, and YasOS libc's printf
family is being annotated (`libs/libc/docs/static_analysis_todo.md`).
Fix the call (`%ld`, a missing argument, `log_msg("%s", user)`).
`-Wformat-n` is an error in the safe default: `%n` writes through a pointer
and is the classic format-string exploit.

### Unused code: `-Wunused-*`

```c
static int helper(void) { return 1; }

int f(int x, int unused_arg)
{
  int tmp;
  int set_only = 0;
  set_only = x;
  x == 1;
  return 0;
}
```

```
$ tcc -ccompat -Wall -Wextra -c unused.c
unused.c:8: warning: statement with no effect
unused.c:6: warning: variable 'set_only' set but not used
unused.c:5: warning: unused variable 'tmp'
unused.c:3: warning: unused parameter 'unused_arg'
unused.c:1: warning: 'helper' defined but not used
```

Silence one on purpose with `(void)x;`, `[[maybe_unused]]` or
`__attribute__((unused))`. Unused parameters are `-Wextra` only.

### `switch`: `-Wswitch`, `-Wimplicit-fallthrough`

```c
enum mode { OFF, ON, AUTO };
int level;

int f(enum mode m, int c)
{
  int level = 0;
  switch (m)
  {
  case OFF:
    level = 1;
  case ON:
    level = 2;
    break;
  }
  switch (c)
  ...
```

```
$ tcc -ccompat -Wall -Wextra -c sw.c
sw.c:10: warning: this statement may fall through
sw.c:11: note: here
sw.c:7: warning: enumeration value 'AUTO' not handled in switch
```

A fall through that is meant says so, with `[[fallthrough]];`,
`__attribute__((fallthrough));` or a `/* fall through */` comment right
before the next label (gcc's `-Wimplicit-fallthrough=3` comments). A
`default:` label covers the missing enumerators.

### Parentheses and empty bodies: `-Wparentheses`, `-Wlogical-not-parentheses`, `-Wdangling-else`, `-Wempty-body`

```c
int f(int a, int b, int c)
{
  if (a = b)
    return 1;
  if (a & b == c)
    return 2;
  if (!a == b)
    return 3;
  if (a)
    if (b)
      return 4;
  else
    return 5;
  if (c);
  return a < b < c;
}
```

```
$ tcc -ccompat -Wall -Wextra -c paren.c
paren.c:3: warning: suggest parentheses around assignment used as truth value
paren.c:5: warning: suggest parentheses around comparison in operand of '&'
paren.c:7: warning: logical not is only applied to the left hand side of comparison
paren.c:7: note: add parentheses around left hand side expression to silence this warning
paren.c:9: warning: suggest explicit braces to avoid ambiguous 'else'
paren.c:14: warning: suggest braces around empty body in an 'if' statement
paren.c:15: warning: comparisons like 'X<=Y<=Z' do not have their mathematical meaning
```

The `else` at line 12 belongs to `if (b)`, whatever its indentation says.
Extra parentheses
(`if ((a = b))`) say an assignment is meant.

### Shadowing: `-Wshadow`

Only when asked, since it is noisy:

```
$ tcc -ccompat -Wshadow -c sw.c
sw.c:6: warning: declaration of 'level' shadows a global declaration
sw.c:2: note: shadowed declaration is here
```

### Signedness and conversions: `-Wsign-compare`, `-Wconversion`, `-Wsign-conversion`

```c
unsigned char to_byte(int v)
{
  return v;
}

int count(int n, unsigned len, double d)
{
  float f = d;
  int i = d;
  if (n < len)
    return i;
  return f > 0;
}
```

```
$ tcc -ccompat -Wextra -c conv.c
conv.c:10: warning: comparison of integer expressions of different signedness: 'int' and 'unsigned int'
$ tcc -ccompat -Wconversion -c conv.c
conv.c:3: warning: conversion from 'int' to 'unsigned char' may change value
conv.c:8: warning: conversion from 'double' to 'float' may change value
conv.c:9: warning: conversion from 'double' to 'int' may change value
```

`-Wsign-compare` is in `-Wextra`. `-Wconversion` (which implies
`-Wsign-conversion` unless that is given too) is in no group: it is
noisy. A cast says a conversion is meant.

### Constant expressions

```c
int f(int x, unsigned u)
{
  int a = 1 << 32;
  int b = x / 0;
  char c = 300;
  int d = 2147483647 + 1;
  if (x == x)
    a++;
  if (u < 0)
    a++;
  return a + b + c + d + (2 << 31);
}
```

```
$ tcc -ccompat -Wall -Wextra -c konst.c
konst.c:3: warning: left shift count >= width of type
konst.c:4: warning: division by zero
konst.c:5: warning: unsigned conversion from 'int' to 'char' changes value from '300' to '44'
konst.c:6: warning: integer overflow in expression of type 'int' results in '-2147483648'
konst.c:7: warning: self-comparison always evaluates to true
konst.c:9: warning: comparison of unsigned expression in '< 0' is always false
konst.c:11: warning: result of '2 << 31' requires 34 bits to represent, but 'int' only has 32 bits
```

(`char` is unsigned on ARM). These are
`-Wshift-count-overflow`, `-Wdiv-by-zero`, `-Woverflow`,
`-Wtautological-compare`, `-Wtype-limits` (`-Wextra`) and
`-Wshift-overflow`. They look only at operations the program performs,
never inside `sizeof` or the dead arm of a constant `?:`.

### Array bounds: `-Warray-bounds`, `-Warray-bounds=2`

```c
int table[8];

int last(void)
{
  return table[8];
}

int sum(void)
{
  int s = 0;
  for (int i = 0; i <= 8; i++)
    s += table[i];
  return s;
}

int scan(void)
{
  int i = 0, s = 0;
  while (i < 10)
  {
    s += table[i];
    i += 2;
  }
  return s;
}
```

```
$ tcc -ccompat -Wall -c bounds.c
bounds.c:5: warning: array subscript 8 is above array bounds of 'int[8]'
bounds.c:12: warning: array subscript 8 is above array bounds of 'int[8]'
$ tcc -ccompat -Warray-bounds=2 -c bounds.c
...
bounds.c:21: warning: array subscript 8 is above array bounds of 'int[8]'
bounds.c:21: note: the subscript is 8 on a path that reaches it
```

Level 1 (in `-Wall`) checks constant subscripts and the plain
`for (i = C; i < N; i++)` loop. Level 2 adds a value-range analysis of each
function: `while` and `do` loops, other strides, conditions, indexes
computed from the loop variable. It costs some compile time (about +14% at
`-O0` on tinycc's own sources), so it is not in `-Wall`. `&a[8]`, one past
the end, is fine.

Level 2 also follows pointers into objects it knows: a local array, struct
or scalar whose address is taken, a global of a known size, a string
literal, and an allocation (`malloc`, `calloc`, `realloc`, or a function
with `alloc_size`), whose size may be a range (`malloc(c ? 16 : 32)`).

```c
int walk(void)
{
  int a[4] = {0}, *p = a, s = 0;
  for (int i = 0; i <= 4; i++)
    s += *p++;
  return s;
}

int heap(void)
{
  int *p = malloc(4 * sizeof *p);
  p[4] = 1;
  return p[0];
}
```

```
$ tcc -ccompat -Warray-bounds=2 -c ptr.c
ptr.c:5: warning: array subscript 4 is outside array bounds of 'int[4]'
ptr.c:3: note: 'a' declared here
ptr.c:5: note: the subscript is 4 on a path that reaches it
ptr.c:12: warning: array subscript 4 is outside the 16 bytes allocated
ptr.c:11: note: allocated here
ptr.c:12: note: the subscript is 4 on a path that reaches it
```

An access is reported when every offset it may have is outside the object,
on a path that certainly runs: not under a branch on a value a loop's
widening over-approximated.  A pointer that never went through a variable
(`*(a + 4)`) is level 1's.  An extern scalar (`extern uint32_t _sdata`) is
taken for a linker's symbol, not an object of 4 bytes, and a global struct
ending in a flexible array is not followed.

### Prototypes: `-Wstrict-prototypes`, `-Wmissing-prototypes`

```c
int old();
int old_style(a) int a; { return a; }
int helper(int x) { return x + old(); }
```

```
$ tcc -ccompat -Wstrict-prototypes -Wmissing-prototypes -c proto.c
proto.c:1: warning: function declaration isn't a prototype
proto.c:2: warning: function declaration isn't a prototype
proto.c:2: warning: no previous prototype for 'old_style'
proto.c:3: warning: no previous prototype for 'helper'
```

`old_style` is also a global function no header declared, like `helper`.
In C23 and yasc, `()` means
`(void)`. The safe default reports old-style definitions only.
`-Wmissing-prototypes` finds functions that should be `static`.

### Casts, packed members, pointer arithmetic

```c
struct __attribute__((packed)) hdr { char tag; int len; };
int *len_of(struct hdr *h) { return &h->len; }
int word(char *buf) { return *(int *)buf; }
char *unconst(const char *s) { return (char *)s; }
void *step(void *p) { return p + 1; }
```

```
$ tcc -ccompat -Wall -Wcast-align -Wcast-qual -Wpointer-arith -c cast.c
cast.c:2: warning: taking address of packed member of 'struct hdr' may result in an unaligned pointer value
cast.c:3: warning: cast increases required alignment of target type
cast.c:4: warning: cast discards 'const' qualifier from pointer target type
cast.c:5: warning: pointer of type 'void *' used in arithmetic
```

These matter on YasOS: a Cortex-M23 faults on every unaligned access, and
an M33 faults on unaligned `LDRD`/`LDM`. `-Waddress-of-packed-member` is in
`-Wall`. `-Wcast-align` works like gcc's `=strict`, and is off by default
because buffers known to be aligned trip it.

### Attributes: `nodiscard`, `deprecated`, `nonnull`, and `sizeof` mistakes

```c
#include <string.h>
[[nodiscard]] int must_check(void);
int old_api(void) __attribute__((deprecated("use new_api")));
void copy(char *d, const char *s) __attribute__((nonnull(1, 2)));
void f(char *p)
{
  must_check();
  old_api();
  copy(p, 0);
  memset(p, 0, sizeof p);
}
```

```
$ tcc -ccompat -Wall -c attrs.c
attrs.c:7: warning: ignoring return value of 'must_check', declared with attribute 'nodiscard'
attrs.c:8: warning: 'old_api' is deprecated: use new_api
attrs.c:3: note: declared here
attrs.c:9: warning: null passed as argument 2, which is declared nonnull
attrs.c:10: warning: argument to 'sizeof' in 'memset' call is the same expression as the destination; did you mean to provide an explicit length?
```

`(void)must_check();` drops a result on purpose. An attribute tcc does not
check (`alloc_size` is checked; `access` is not yet) is reported by
`-Wignored-attributes` in the safe default.

## 4. Buffer overflows and object sizes

tcc knows the size of declared objects, members, string literals and
allocations with a constant size, and checks the string and memory
functions against them (`-Wstringop-overflow`, `-Wformat-overflow`,
`-Wformat-truncation`, all in `-Wall`):

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct rec { char name[8]; int id; };

void fill(struct rec *r, int n)
{
  char buf[16];
  memcpy(buf, "0123456789abcdefXYZ", 20);
  strcpy(r->name, "too long a name");
  sprintf(buf, "record %d of %d", n, n);
  snprintf(buf, 4, "%d", 12345);
  puts(buf);
}

void *table(unsigned n)
{
  void *big = malloc(0x90000000u);
  void *rows = malloc(n * sizeof(struct rec));
  free(big);
  return rows;
}

void release(void)
{
  struct rec r;
  free(&r);
}
```

```
$ tcc -ccompat -Wall -Walloc-mul-overflow -c objsz.c
objsz.c:10: warning: 'memcpy' writing 20 bytes into a region of size 16 overflows the destination
objsz.c:11: warning: 'strcpy' writing 16 bytes into a region of size 8 overflows the destination
objsz.c:11: note: use strlcpy(r->name, src, 8), which bounds the copy
objsz.c:13: warning: 'snprintf' output truncated: at least 6 bytes into a destination of size 4
objsz.c:19: warning: argument 1 value '2415919104' exceeds maximum object size 2147483647
objsz.c:20: warning: the size argument of 'malloc' is a multiplication that may overflow: use 'calloc' or 'reallocarray'
objsz.c:28: warning: 'free' called on unallocated object 'r'
```

`sprintf` is checked with each unknown value at its shortest: here
"record 0 of 0" fits, so it is quiet. When the size is exact the note
gives the bounded call to write instead, and section 8 shows how to apply
it automatically. `-Wformat-truncation` reports an `snprintf` whose result
is unused; `-Wformat-truncation=2` reports it whether the result is used or
not.

Allocations and frees are checked too: `-Walloc-size-larger-than=N`
(line 19), `-Wfree-nonheap-object` (28) and `-Wmismatched-dealloc` are in
`-Wall`; `-Walloc-mul-overflow` (20) only when asked, since a test of `n`
before the call is not seen. A deallocator is known from the allocator's
declaration, as YasOS libc declares `fopen`:

```c
typedef struct file FILE;
int fclose(FILE *f);
void free(void *p);
FILE *fopen(const char *path, const char *mode) __attribute__((malloc, malloc(fclose, 1)));

void done(void)
{
  FILE *f = fopen("log", "r");
  free(f);
}
```

```
$ tcc -ccompat -Wall -c dealloc.c
dealloc.c:9: warning: 'free' called on pointer returned from a mismatched allocation function
dealloc.c:8: note: returned from 'fopen'
```

**`__builtin_object_size(p, type)`** and
**`__builtin_dynamic_object_size`** give the same sizes to the program
(gcc's types 0 to 3, `(size_t)-1` or 0 when unknown). tcc decides them
while parsing, so the answer is the same at every `-O`:

```c
struct rec r; char buf[16]; char *p = malloc(10);
__builtin_object_size(r.name, 1)    /* 8: the member */
__builtin_object_size(&r, 0)        /* 12: the whole struct */
__builtin_object_size(buf + 4, 0)   /* 12: what is left */
__builtin_object_size(p, 0)         /* 10: the allocation */
```

A C library can build `_FORTIFY_SOURCE` wrappers on it; tcc folds
`__builtin___memcpy_chk` and friends to the plain call when the size is
provably enough.

### Fortify from the declaration: no macro

A declaration can say what its pointer arguments must hold, and tcc checks
every call against it where the caller names the buffer, with no wrapper
macro and no `_chk` entry in the library:

```c
char *dirname(char *path) __attribute__((null_terminated_string_arg(1)));
void fill(char *d, size_t n) __attribute__((access(write_only, 1, 2)));
```

- `null_terminated_string_arg(n)`: argument n's NUL has to be inside its
  buffer (the closest member). Before the call, `__tcc_fortify_str(p, size)`
  ends the program through `__chk_fail` when there is none (libtcc1's, weak:
  "*** buffer overflow detected ***: terminated" through write(2), then
  abort; a program's or a C library's own wins); a null pointer passes. A literal or a `const` array is not checked, and a local buffer
  whose bytes the function wrote is dropped from `-O1`.
- `access(mode, ref, size)`: argument `size` elements of argument ref's
  pointed-to type fit its buffer (the whole object): `cmp` + `udf #254`
  before the call. A constant that fits costs nothing, one that does not is
  also `-Wstringop-overflow`; a VLA or `malloc(n)` is checked at run time.

yasc says the same in the parameter list: `char path[:0]`, `char d[n]` with
n a parameter (before or after d), `const void buf[n]` for bytes, `out`
for write-only (the yasc cheatsheet, "Parameters that say their buffer").

A buffer tcc cannot see (a pointer parameter) is not checked.  On in the
safe default; under `-ccompat` when `_FORTIFY_SOURCE` is defined and not 0,
or with `-ffortify`; `-fno-fortify` turns it off (a check per call: 2
instructions for a size, a call for a string).  YasOS libc's `libgen.h`
declares `dirname` and `basename` this way.

### strcpy, strcat, sprintf: checked by name

No argument gives their destination's size, so no attribute can say it:
with fortify on, tcc knows the C library's `strcpy`, `stpcpy`, `strcat`,
`sprintf` and `vsprintf` by name (gcc's builtin knowledge; not a `static`
one, not one this unit defines, not under `-fno-builtin`).  Where the
caller names the destination (its object's size, the closest member):

- `strcpy` / `stpcpy` / `strcat`: before the call (whose arguments, checks
  and yasc contract stay those of the plain call) libtcc1's
  `__tcc_fortify_strcpy(src, size)` / `__tcc_fortify_strcat(dst, src,
  size)` end the program through `__chk_fail` when the string (and, for
  strcat, what is there already) does not fit.  A literal known to fit, or
  `""` appended, costs nothing.
- `sprintf` / `vsprintf`: the call becomes `__builtin___sprintf_chk` with
  that size -- the plain function when the output provably fits, else
  libtcc1's `__tcc_sprintf_chk` / `__tcc_vsprintf_chk`, which format into the
  size and end the program when the output needed more (nothing past the
  destination is written).  The arguments are converted as the plain
  call's.

A constant size that does not fit gets one warning: the check's (`... the
call ends the program`), not the plain function's `-Wstringop-overflow` as
well.  yasos libc also marks these functions `yasc_unsafe`: yasc code calls
them inside `unsafe` only (`-Wyasc-unsafe-call`, with the fix-it to
`strlcpy(d, s, sizeof d)` / `snprintf(d, sizeof d, ...)`).

## 5. Flow checks

These follow every path through a function (`if`, loops, `goto`, early
`return`) on tcc's IR before any optimisation, so their answers do not
depend on `-O`.

### yasc ownership: `-Wlifetime`

In yasc, each pointer says whether it owns its target, and the checks hold
the code to it: leaks, double frees, use after free, a `?` pointer
dereferenced untested, a borrow outliving its owner, a handle never closed.
With YasOS libc's headers, whose allocators carry the attributes:

```c
#include <stdlib.h>
#include <string.h>

char *?owned dup_upper(const char *s)
{
  char *?owned d = malloc(strlen(s) + 1);
  strcpy(d, s);
  return d;
}

void twice(void)
{
  char *?owned p = malloc(4);
  free(p);
  free(p);
}
```

```
$ tcc -c own2.c
own2.c:7: warning: a pointer that may be null is passed as argument 1 of 'strcpy', which is not '?'
own2.c:15: warning: 'p' is freed twice
own2.c:14: note: 'p' was freed here
```

This is yasc's own check (it is on in the safe default, and needs the
dialect). [The yasc tutorial](../examples/yasc/README.md) teaches it
chapter by chapter: chapter 3 lists every mistake it catches, chapter 10
covers arrays of owned pointers and file-descriptor handles. Converting C
code is helped by the plain-pointer error's note, which says whether the
body frees the pointer (`*owned`), only borrows it (`*view`) or tests it
for null (`?`), with a fix-it (section 8).

A parameter says what it does with what it points to, before its type:
nothing (or `in`) reads it -- the target of a `T *view` parameter is
const -- `inout` reads and writes it, `out` writes it before it reads it.
Writing a read-only target is an error in yasc, so is handing it to a
pointer that may write it.  `out` is checked in the body: `'*p' is read
before it is written`, `'*p' is not written when 'f' returns`.  A
declaration and a definition that disagree on any of this (or on owned /
view / maybe / drop / borrows) are an error: `'f' is declared again with
another yasc contract: parameter 1 ('p') differs in ...`.

A pointer handed back through an out parameter says where it points the
same way a result does: `long strtol(const char *s, out char *
borrows(s) *?end, int base)`.  The callee may store a pointer
into `s` through `end` (any other view parameter stored there still
escapes), and the caller's `char *view e` whose address it passed borrows,
after the call, from the owned variable passed for `s` -- `free(buf)` then
`*e` is reported as a use after free.  Only an `out` parameter's target
says it (an in/out one may hand back what it read).  A callee in the same
unit that is inlined (`-O1` and up) leaves `e` unfollowed, so nothing is
reported there.

Two more yasc errors, both lowered only by `unsafe`: a `view` / `owned`
pointer read through a union member is a regular-C pointer (`-Wunchecked`;
an owned member in a union is refused outright), and a bit-field store to
volatile memory, a hidden read-modify-write of the whole register, is
`-Wvolatile-bitfield` (the tutorial's "Unions and hardware registers").
A tagged union, `union(k) { T m when(K); ... } u;`, is checked instead: a
member is used only where its struct's tag `k` is known to select it
(`-Wlifetime`).  Register fields say `ro`, `wo` or `w1c` after their
declarator: an `ro` one written, a `wo` one read, or a `w1c` one written
back as it was read (`status |= X`) is `-Wregister-access`.

### yasc enforces initialization

In yasc (under `-Wlifetime`: the safe default, where only `unsafe { }`
lowers it) every local has a value before anything can see it:

```c
void fill(out int *p);
void keep(int *p);

int f(int c)
{
  int x;
  if (c)
    x = 1;
  int n;
  fill(&n);           /* fine: an out parameter writes it */
  int m;
  keep(&m);           /* keep may read it */
  char buf[64];
  return x + n + m + buf[0];
}
```

```
$ tcc -c init.c
init.c:13: error: 'buf' has no value: yasc does not follow what is stored in an array later: initialize it, or write '= undefined'
init.c:12: error: the address of 'm' is taken where it may have no value: set it first, pass it to an 'out' parameter, or declare it '= undefined'
init.c:14: error: 'x' may be used uninitialized: yasc wants a value on every path
```

- A scalar read on a path where nothing stored it is an error, the "may"
  case too: the `-W[maybe-]uninitialized` check below, made strict, at every
  `-O` level.
- A scalar's address may be taken only where it has a value, unless it goes
  straight to an `out T *` parameter (a call's, or an inlined callee's), or
  is only stored through (`ckd_add(&r, a, b)`), which writes it.
- An array, a struct, a union or a complex value, whose later stores the
  check does not follow, needs an initializer.
- An initializer may not read the variable it initializes: `int x = x + 1;`
  and `u = { .value = v, .parts = { .imag = -u.parts.imag } }` are errors
  (the member is not set yet, and the elements are evaluated in no set
  order). Its address (`void *p = &p;`), `sizeof`, and a call whose result
  the callee writes straight into it are not reads.
- `T x = undefined;` declares an automatic local with no value on purpose
  and emits no code: the array or struct is filled later, or the scalar's
  address may go somewhere before it has a value (`sscanf(s, "%d", &n)`).
  Reads of a scalar are still checked. `undefined` is an ordinary name
  wherever one by that name is in scope, and in regular C.

Under `-ccompat` none of this applies: a yasc file still accepts
`= undefined`, and `-Wlifetime` turns the rules on as warnings.

### Plain C: uninitialised values, addresses of locals, null

These work on any C. One file shows all four:

```c
#include <stddef.h>
int *saved;

int pick(int c)
{
  int x;
  if (c)
    x = 1;
  return x;
}

int *address(void)
{
  int local = 0;
  return &local;
}

void keep(void)
{
  int local = 0;
  saved = &local;
}

int first(void)
{
  int *p = NULL;
  return *p;
}
```

```
$ tcc -ccompat -Wall -Wmaybe-uninitialized -Wdangling-pointer -Wnull-dereference -c flow.c
flow.c:9: warning: 'x' may be used uninitialized
flow.c:15: warning: 'address' returns the address of a local variable
flow.c:21: warning: the address of a local variable is stored in global 'saved', which outlives 'keep'
flow.c:27: warning: a null pointer is dereferenced
```

**`-Wuninitialized`, `-Wmaybe-uninitialized`:** `x` is set on one path only (line 9). Under `-ccompat`,
`-Wmaybe-uninitialized` reports the "may" case and `-Wuninitialized` the
certain one; a local whose address is taken is not followed. The safe
default is yasc, which enforces initialization (above): there both are
errors. A variable declared in a loop body
is uninitialised again at each iteration. A local read in its own initializer
(`int n = n + 1;`, `struct pt p = { .x = 1, .y = p.x };`) is reported by
`-Wuninitialized` too.

**`-Woverride-init`** (in `-Wextra` and the safe default): an initializer
element naming bits an earlier one already set loses the earlier value:
`{ .x = 1, .x = 2 }`, `{ [2] = 1, [1] = 2, 3 }` (the `3` lands on `[2]`). A
union holds only the member initialized last, so `{ .i = v, .p.hi = 3 }`
drops `.i` although the two share no byte, and in
`{ .value = v, .parts = { .imag = 1 } }` `.parts.real` is zero, not
`v`'s real part. Bit-fields count bits: `{ .a = 1, .b = 2 }` in one storage
unit is fine.

**`-Wreturn-local-addr`, `-Wdangling-pointer`:** `return &local;` (line 15, `-Wall`) and `saved = &local;` with `saved` a
global (line 21, the safe default). A store through a parameter
(`*slot = &local`) is reported too; a global cleared again before the
function returns is not. In yasc the address is followed through a call
whose result borrows from the argument it went to: `return f(buf);` with
`char *borrows(p) f(char *p)` (or the elision) is reported.

**`-Wnull-dereference`:** `*p` with `p` null (line 27). This check runs after optimisation, so it
finds more at `-O1` and up (a callee returning null, once inlined), and its
results may differ by level. In yasc, `-Wlifetime` reports a dereference of
anything declared `?`, at every level.

### Released pointers: `-Wuse-after-free`

In `-Wall`.  After a deallocator (`free`, or the one an allocator's
`malloc(dealloc, i)` attribute names) is called with a pointer variable, a
load or store through it, or a second release, is reported when the
release happened on every path to it:

```c
struct node { struct node *next; int v; };

int last(struct node *n)
{
  free(n);
  return n->v;
}

void twice(char *p)
{
  free(p);
  free(p);
}
```

```
$ tcc -ccompat -Wall -c uaf.c
uaf.c:6: warning: 'n' is used after it was freed
uaf.c:12: warning: 'p' is freed twice
```

Only the pointer that was released is followed (and copies made after
it): `q = p; free(p); *q` is not seen, nor a release on some paths only.
`realloc` does not count, since its argument lives on when it fails.  In
yasc, `-Wlifetime` does this for owned pointers, and this check stays
off.

### Untrusted formats: `-Wtainted-format`

```c
#include <stdio.h>
#include <stdlib.h>

static void say(const char *msg)
{
  printf(msg);
}

int main(int argc, char **argv)
{
  char line[64];
  if (fgets(line, sizeof line, stdin))
    printf(line);
  say(getenv("GREETING"));
  return argc > 1 ? printf(argv[1]) : 0;
}
```

```
$ tcc -ccompat -Wtainted-format -c taint.c
taint.c:13: warning: the format of 'printf' comes from untrusted input: write printf("%s", ...)
taint.c:6: warning: the format of 'printf' comes from untrusted input: write printf("%s", ...)
taint.c:14: note: untrusted input reaches it through the call to 'say' in 'main'
taint.c:15: warning: the format of 'printf' comes from untrusted input: write printf("%s", ...)
```

Untrusted input is `argv`, `getenv`, and what `fgets`, `read`, `recv`,
`getline` and the like fill, followed through variables, struct members,
globals and the functions of the unit. Your own sources and sinks can be
marked with `__attribute__((tainted_result))` and `tainted_arg(n)`. It is
off by default (gcc has no such check).

## 6. Across units: `-Wcross-tu`

A header that does not match its definition compiles cleanly in both files
and breaks at run time. With `-Wcross-tu` on every compile and on the link,
the link compares the types each unit saw:

```c
/* api.h */                         /* b.c */
int scale(int v);                   struct point { int x; long y; char tag; };
struct point { int x, y; };         struct point origin;
extern struct point origin;         long scale(int v) { return v * 2L; }
```

```
$ tcc -ccompat -Wcross-tu -c a.c && tcc -ccompat -Wcross-tu -c b.c
$ tcc -ccompat -Wcross-tu a.o b.o ... -o prog
tcc: warning: 'struct point' has different layouts at b.c:1 and at api.h:2 in a.c ('origin' uses it): member 'y' is 'long' at offset 4 in b.c, 'int' at offset 4 in a.c
tcc: warning: 'scale' declared as 'int (int)' at api.h:1 in a.c, defined as 'long (int)' at b.c:3
```

The records go into a section the link drops; objects compiled without the
option are unchanged. `-Werror=cross-tu` at the link fails it. Units are
told apart by file name.

## 7. Many errors in one run

After an error tcc goes on parsing, so a run shows several at once, up to
`-fmax-errors=N` (8 by default, 0 for no limit). The compile still fails
and writes no object.

```
$ tcc -fmax-errors=1 -c plain.c
plain.c:6: error: implicit declaration of function 'count'
compilation terminated due to -fmax-errors=1.
```

Scripts that count errors (the yasos ratchet, `scripts/safe_mode_ratchet.py`)
should pass `-fmax-errors=0`.

**`-fsyntax-only`** runs the parse and every check above but generates and
writes nothing: the quickest way to check a file or a tree.

**On a terminal** each diagnostic comes in gcc's colours (the place bold,
`error:` red, `warning:` magenta, `note:` cyan, 'quoted' names bold), with
the option that controls it and the source line under it:

```
fix.c:5: warning: 'sprintf' writing at least 11 bytes into a region of size 8 overflows the destination [-Wformat-overflow]
    5 |   sprintf(buf, "%s-%d!", "toolong", n);
      |   ^~~~~~~
fix.c:5: note: use snprintf(buf, sizeof buf, ...), which bounds the output
```

(A note about the line just shown does not show it again.)

tcc keeps no column: the caret marks the first 'quoted' name of the message
found exactly once on the line; with none, the line is shown alone.
Through a pipe or into a file (a log, an editor's parser, a test) the text
stays one plain line per diagnostic, unless asked for:

| option | what it does | default |
|---|---|---|
| `-fdiagnostics-color[=always\|never\|auto]`, `-fno-diagnostics-color` | the colours | auto: a terminal, `NO_COLOR` unset, `TERM` not `dumb` |
| `-f[no-]diagnostics-show-caret` | the source line under a diagnostic | a terminal (`TERM` not `dumb`) or `-fdiagnostics-color=always` |
| `-f[no-]diagnostics-show-option` | `[-Wname]` after a warning, `[-Werror=name]` after one made an error | as the source line |
| `-f[no-]diagnostics-show-line-numbers` | the line's number beside it | on |
| `-fdiagnostics-plain-output` | no colour, no source line, no `[-Wname]` | |

`tcc_set_error_func()` gets the plain line.

## 8. Output for tools: JSON, SARIF, fix-its

`-fdiagnostics-format=json` writes gcc's JSON, and
`-fdiagnostics-format=sarif` writes SARIF 2.1.0 (for CI annotations and
code-scanning tools), on stderr. Each diagnostic carries its option, its
notes, and any fix-its:

```
$ tcc -ccompat -Wall -fdiagnostics-format=json -c fix.c
[{"kind": "warning",
  "message": "'sprintf' writing at least 9 bytes into a region of size 8 overflows the destination",
  "option": "-Wformat-overflow",
  "locations": [{"caret": {"file": "fix.c", "line": 6}}],
  "children": [{"kind": "note", "message": "use snprintf(buf, sizeof buf, ...), which bounds the output", ...}],
  "fixits": [{"start": {"file": "fix.c", "line": 6, "column": 3},
              "next": {"file": "fix.c", "line": 6, "column": 10}, "string": "snprintf"},
             {"start": {"file": "fix.c", "line": 6, "column": 14},
              "next": {"file": "fix.c", "line": 6, "column": 14}, "string": ", sizeof buf"}]}]
```

(reformatted: tcc writes each diagnostic on one line). In SARIF each result's
`ruleId` is the option (`"-Wformat-overflow"`), notes become
`relatedLocations` and fix-its become `fixes`. Locations have lines but no
columns, since tcc keeps none, except in fix-its.

`-fdiagnostics-parseable-fixits` prints the fix-its in gcc's text format,
for editors and scripts to apply:

```
fix.c:6: warning: 'sprintf' writing at least 9 bytes into a region of size 8 overflows the destination
fix.c:6: note: use snprintf(buf, sizeof buf, ...), which bounds the output
fix-it:"fix.c":{6:3-6:10}:"snprintf"
fix-it:"fix.c":{6:14-6:14}:", sizeof buf"
```

Fix-its are given for `sprintf`/`vsprintf`/`strcpy`/`strcat` into an
object of known size (bounded call), for deprecated libc functions with a
replacement, for yasc's "write move(p)", and for a plain pointer in yasc
(the `*owned` / `*view` / `*?` the body implies).

## 9. Stack budgets

A YasOS process stack is 4 to 8 KB, and stack overflows have been real
bugs. Four tools, all off by default, keep it bounded
([`examples/checks/02_stack_budget.c`](../examples/checks/02_stack_budget.c)
uses them all):

```c
#include <alloca.h>
int use(char *p);
int f(int n)
{
  char *p = alloca(n);
  char v[n];
  return use(p) + use(v);
}
int g(void)
{
  char big[6000];
  return use(big);
}
```

```
$ tcc -ccompat -Wvla -Walloca -Wframe-larger-than=4096 -Wstack-usage=4096 -c stk.c
stk.c:5: warning: use of '__builtin_alloca'
stk.c:6: warning: variable length array 'v' is used
stk.c:8: warning: stack usage of 'f' might be unbounded
stk.c:13: warning: the frame size of 'g' is 6024 bytes, larger than 4096 bytes
stk.c:13: warning: stack usage of 'g' is 6024 bytes, larger than 4096 bytes
```

- `-Wvla`, `-Walloca` (both in the safe default): frames that nothing
  bounds.
- `-Wframe-larger-than=N`: one function's frame, as generated (saved
  registers, locals, spills, outgoing arguments). The number depends on
  `-O`; `-O0` frames are larger.
- `-Wstack-usage=N`: the same, plus "might be unbounded" for every dynamic
  frame.
- `-fstack-usage` writes `x.su` next to `x.o`, gcc's format:
  `02_stack_budget.c:21:21:checksum_big	2064	static`.

A function's own frame is half the story: the depth is the sum along the
deepest call path. `-fcallgraph-info=su` writes `x.ci`, which also lists
each function's calls (direct, tail, and indirect by type), and
`scripts/stack_usage.py` adds the frames up from each entry point:

```
$ python3 scripts/stack_usage.py --libtcc1 --unknown-budget 256 \
      -e run_unbounded -e run_bounded 02_stack_budget.ci
run_unbounded: UNBOUNDED (recursion, dynamic); 2344 bytes on the deepest bounded path
         24      24  run_unbounded  [static]
       2064    2088  checksum_big  [static]
        256    2344  __tcc_memset  [budget]
run_bounded: 296 bytes
         24      24  run_bounded  [static]
         16      40  checksum_static  [static]
        256     296  __tcc_memset  [budget]
```

Recursion and dynamic frames make a path UNBOUNDED. A callee no `.ci`
describes (assembly, a library built without the option) is UNKNOWN, and
the result is a lower bound unless `--unknown-budget N` or `--budget
NAME=N` prices it. `--libtcc1` reads the compiler runtime's `.ci`, and an
archive argument (`libc.a`) reads the `.ci` beside it. Calls through a
pointer go to the worst address-taken function of the same calling shape.
`--limit N` exits 1 above N, unbounded or unknown, which makes a CI gate
of it. `python3 scripts/stack_usage.py -h` documents the format and the
cases the resolution cannot see. For a YasOS app, `-e main` gives the
YAFF `-stack-size` to link with.

## 10. Run-time checks

These change the generated code, so they are off by default. Without the
option, objects are exactly what they would be otherwise.

### Undefined behaviour: `-fsanitize=undefined`

The compiler adds a test before each operation C leaves undefined: signed
`+ - *`, negation, `++`/`--` and compound assignment overflowing, `INT_MIN
/ -1`, division by zero, a shift count `< 0` or `>=` the width, a negative
or overflowing signed `<<`, a null or misaligned dereference of a computed
pointer, a VLA bound `<= 0`, an index outside an array of known size. The
sub-options are gcc's names: `signed-integer-overflow`, `shift`
(`shift-base`, `shift-exponent`), `integer-divide-by-zero`, `null`,
`alignment`, `vla-bound`, `bounds`, `unreachable` (a `__builtin_unreachable()`
that is reached; yasc's `unreachable` is checked without it, and with it
reports through the handler); `-fno-sanitize=` takes some away.

**Trap mode** (the default) needs no runtime library: a failed check
executes `udf #254`. On the board that is a fault; under a debugger
(`ptrace`) it is a stop at the check.

```sh
tcc -fsanitize=undefined prog.c -o prog
```

**Handler mode** calls libc's `__ubsan_handle_*` (clang's ABI, in YasOS
libc's `ubsan.c`), which prints the place and the values on stderr:

| Option | After a report |
|---|---|
| `-fsanitize-recover=undefined` or `-fno-sanitize-trap=undefined` | the program goes on |
| `-fno-sanitize-recover=undefined` | the program aborts |

`-fsanitize-trap[=...]` wins over them in any order.
[`examples/checks/03_sanitizers.c`](../examples/checks/03_sanitizers.c)
shows both modes:

```
step 1
03_sanitizers.c:55: runtime error: load of null pointer of type 'int'
load: read
step 2
03_sanitizers.c:23: runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'
add: -2147483648
```

Operations folded at compile time are compile-time warnings instead
(section 3); pointer arithmetic and conversions are not checked.

**Array parameters.** `bounds` also checks a parameter declared with a
size, C99's syntax that C itself ignores (the parameter is a pointer), as
clang's `__counted_by` does:

```c
int get(size_t n, const int p[n], size_t i) { return p[i]; }   /* i < n */
void name(char s[16], int i) { s[i] = 0; }                      /* i < 16 */
int first(const int p[static 4]) { return p[5]; }               /* not checked */
```

The size is evaluated on entry, with where `p` points then; a subscript of
`p` (or of `p` moved: `p++; p[n - 1]` traps) is checked against it. `[static
N]` (at least N), `[]`, `[*]` and plain pointers say nothing of an end and
are not checked. The ABI does not change: callers pass the pointer and the
size as before. The test is one compare, branch and `udf`; a loop whose own
condition bounds the index (`for (i = 0; i < n; i++) p[i]`, signed from 0 or
unsigned) needs none, and neither does an index already within range (`buf[k
& 15]` for `buf[16]`). An inlined body keeps the checks of its constant-size
parameters; a function with a `p[n]` parameter is not inlined.

**`-ftrapv`** is the arithmetic subset (`+ - *` and negation), always as
traps.

### Stack smashing: `-fstack-protector`, `-strong`, `-all`, `-explicit`

A canary below the saved registers, checked on return; a mismatch calls
`__stack_chk_fail` (libc's, which aborts). Which functions get one follows
gcc's rules: `-fstack-protector` covers functions with character buffers
or `alloca`, `-strong` also any local array or local whose address is
taken, `-all` every function, and `-explicit` only the functions marked
`__attribute__((stack_protect))`. A protected function makes no tail call.
`__attribute__((no_stack_protector))` exempts one function from any of
them.

### Bounds checking: `-b` (QEMU only)

`-b` costs about 20 times the instructions: every access calls the runtime,
which searches every region (`70 + 13 * log2(regions)` instructions, the
region table in the board's PSRAM). On the board, use `-fsanitize=bounds`
(above) instead: its checks need the size in the type, and cost a compare.

`tcc -b` instruments every pointer arithmetic and dereference against the
regions of locals, globals and `malloc` blocks, and reports out-of-region
accesses, use after free and bad frees (`BCHECK: 0x... is outside of the
region (0x...0x...)`, then exit 255). The runtime (`lib/bcheck-bm.c`)
is for newlib on QEMU: the cross compiler has `-b`, the compiler on the
board does not. VLAs, `alloca`, address-taken scalars and the C library's
own accesses are not checked yet (`static_analysis_todo.md`, "Open items").

## 11. Still being extended

Work in progress on other branches (round 5):

- `-b`: VLAs, `alloca`, address-taken scalars, libc's string functions.
- `-Wformat`: `%N$` positional operands.
- `-Wtainted-format`: summaries across units.

Known gaps that are documented rather than in progress: no thread-safety
annotations yet (YasOS has no threads); columns in diagnostics; the
limits listed in [`static_analysis_todo.md`](static_analysis_todo.md) ("What the
flow checks do not follow", "Open items").
Open bugs found in the checks are in [`bugs/`](bugs/README.md).
