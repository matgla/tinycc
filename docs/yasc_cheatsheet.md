# yasc cheat sheet

One page of the dialect. The tutorial is `examples/yasc/README.md`; the
design and its limits are in `docs/static_analysis_todo.md`.

## Turning it on

| | |
|---|---|
| `tcc prog.c` | yasc + `-Wall` + `-Wlifetime`, lifetime mistakes are errors (the safe default) |
| `tcc -ccompat old.c` | regular C; the yasc words are identifiers |
| `#pragma yasc` | this file is yasc, even under `-ccompat` |
| `-iyasc DIR` | headers found there are yasc |
| `#pragma tcc c_begin` / `c_end` | a regular-C section of a yasc file (an `#include` in it stays C) |
| `tcc -yasc-c-header yasc/x.h -o x.h` | the C header of a yasc header (docs/yasc_c_headers.md) |
| `tcc -fsyntax-only f.c` | every check, no output |

The dialect belongs to a file: an `#include`d header is regular C unless it
says `#pragma yasc` (or comes from `-iyasc`).

## Pointers: four questions, four words

Every `*` says `owned` or `view` (or `borrows(...)`); the word goes **after**
the `*` it qualifies. `maybe`, `restrict` and `const` combine with either.
A parameter's own pointer (the last `*` before its name) is the exception: it
is a `view` unless it says `owned` or `borrows(...)` -- the most a callee can
do with it is look -- so a parameter writes only what changes hands (`owned`,
`drop ... owned`), `maybe`, `inout` / `out`. Writing `view` there anyway is
`-Wredundant-view`. A local pointer variable likewise is a `view` (one that
may hold null: what it holds is what flows into it) unless its function owns
it -- gives it an owned result (`malloc(n)`), frees it, hands it to an
`owned` / `drop` parameter, stores or returns it as owned -- and then it says
`owned`. `T * view p` on a local says more (never null); `view maybe` there
is `-Wredundant-view`. Inner levels, arrays of pointers, statics, results,
members and globals still say it.

| Question | Answer | Write |
|---|---|---|
| Who frees it? | this pointer, once | `T * owned p` |
| | someone else; it does not leave the function | `T * view p` (a parameter or local: `T *p`) |
| | someone else; it lives as long as `x` | `T * borrows(x) p` (results, members) |
| Can it be null? | no | (nothing; a local: `T * view p`) |
| | yes: test it before use | `T * view maybe p` (a parameter: `T * maybe p`; a local: `T *p`) |
| Can others reach it meanwhile? | yes (shared) | `T * view p` |
| | no: an exclusive borrow, until its last use | `T * restrict view p` (a local: `T * restrict p`) |
| Can I write through it? | a parameter: no, unless it says so | `T *p` (in), `inout T *p`, `out T *p` |
| | a local or member: yes | `T *p` (a member: `T * view p`) |
| | a local or member: no | `const T *p` (a member: `const T * view p`) |

`view` is about ownership only. A view *parameter's* target is read-only
unless the parameter says `inout` (or `out`, written before it is read) --
writing it, or handing it on where something may write it, is an error.
Locals and members write through a view freely. Read-only elsewhere is
`const`, exclusive is `restrict` (yasc has no `readonly` word any more: it
was `const T * view`, and is an error after a `*`). `restrict` borrows, it does not take
ownership: no free through it, and the owner is usable again after its last
use.

Each level says what it is:

```c
char * owned * view argv_copy;       /* a view of an array of owned strings */
void f(char * view args[]);          /* same as char * view *args: the inner level says it */
void g(char * view args[owned]);     /* C99-style: the brackets qualify that pointer */
void h(int a[maybe 4]);              /* also [view], [restrict], [const static 2] */
int main(int argc, const char * view *argv);  /* in: argv[i] is not reassigned */
```

A string literal, `&x` and an array decaying in an expression are views.
Function pointers are exempt (they point at code): `int (* view cmp)(...)`
still says that one is never null.

The null pointer is `nullptr`, and only that: `NULL`, `0` and `(T *)0` used as
a pointer (stored, passed, returned, compared, in `?:` or an initializer)
draw `-Wzero-as-null-pointer-constant`, a warning ("write nullptr"). `= {}`
zeroes a struct. `!p` and `if (p)` are fine. A system header's macros
(`SIG_DFL`) stay quiet; what yasc code writes inside a macro's arguments
(`assert(p != NULL)`) does not.

## Every local has a value

yasc enforces initialization (errors; `unsafe { }` lowers them):

```c
int x;                     /* fine while every path sets it before a read */
if (c) x = 1; else x = 2;
int n;   fill(&n);         /* fill(out int *p): an out parameter sets it */
int m = 0; keep(&m);       /* any other address needs a value first */
char buf[64] = {};         /* arrays, structs, unions: an initializer ... */
struct stat st = undefined;  /* ... or `= undefined`: filled later, no code */
int k = undefined; sscanf(s, "%d", &k);  /* its address may go out unset */
```

A scalar read where some path left it unset is an error ("may be used
uninitialized" too). `= undefined` is only for automatic locals and names
nothing special where a variable `undefined` is in scope.

## Before the type: what a function does with a parameter

| Write | Meaning |
|---|---|
| `T *p` / `in T *p` | the callee only reads `*p` (a view, its target const: the default) |
| `inout T *p` | the callee reads and writes `*p` (a method that changes `self`, `FILE *`, a buffer it appends to) |
| `out T *p` | the callee writes `*p` before returning, never reads it first |
| `out T * borrows(s) *p` | ... and what it writes there points into `s` (strtol's end): the caller's variable borrows from what it passed for `s` |
| `T * owned p` | the callee takes `p` over and keeps or hands it on |
| `drop T * owned p` | the callee consumes `p` (frees it or hands it on) |
| `drop struct S *self` | the callee frees `self`'s owned fields, not `self` |

## Moving and borrowing

| Write | Meaning |
|---|---|
| `move(p)` | ownership leaves `p` (to an owned variable, member, parameter); `p` owns nothing after |
| `borrow(p)` | a view of `p`'s memory; `p` keeps owning it |
| `return p;` / `free(p)` | moves without `move()` (a whole local, a `drop` parameter) |
| `return move(s->buf);` | a member or global still needs it |
| `auto q = p;` | error with `p` owned: say `move(p)` or `borrow(p)` |

## Results

| Write | Meaning |
|---|---|
| `T * owned maybe f(...)` | the caller owns the result, null on failure |
| `T * view f(T *s)` | borrows from the view parameters (the elision) |
| `T * borrows(a) f(T *a, T *b)` | borrows from `a` only |
| `const char * borrows(static) name(int e)` | points to static storage |
| `void * owned maybe takes(p) grow(void * owned p, unsigned n)` | on success the result took over `p`; on null `p` is still yours |

A view result with no view parameter is an error: write `borrows(static)`.

### The C API beside the yasc one: `c_api(T)`

`c_api(T)` after a `*` is what C callers see in place of everything to its left;
the rest of the declarator stays (docs/yasc_c_api.md):

| Write | yasc sees | the C header says |
|---|---|---|
| `const char * borrows(static) maybe c_api(char *) dlerror(void)` | `const char *` | `char *dlerror(void)` |
| `int execv(const char *p, const char * view const c_api(char * const) argv[])` | `const char * const argv[]` | `char * const argv[]` |
| `out const char * borrows(s) c_api(char *) * maybe endptr` | `const char **` | `char **endptr` |

T differs in const and volatile only (never a pointer for an int, int for long);
C code that writes through what such a function hands it is `-Wyasc-c-api`.

## Deallocators and handles

```c
FILE * owned(fclose) maybe f = fopen(path, "r");    /* released by fclose, not free */
int owned(close) maybe fd = open(path, O_RDONLY);    /* an int handle; invalid = < 0 */
int slot_read(int view h);                           /* borrows a handle */
int n = cast<as(int)>(fd);                           /* its number: a copy that owns nothing */
regex_t owned(regfree) re;                           /* an object owned as a whole */
```

The wrong deallocator is an error. For a `maybe` handle the tests are `fd < 0`,
`fd == -1` and `fd >= 0`, not `if (fd)`.

An `int` is a number: it points at nothing, so it is never a view by default.
A handle goes only where the type says it is one -- `int view fd` (borrowed),
`int owned(close) fd` (taken), `int view maybe dirfd` (AT_FDCWD or -1 fine).
Storing one in a plain `int` (a parameter, a local, a member, a global, a
result) is an error; `cast<as(int)>(fd)` takes its number on purpose. A regular-C
parameter cannot say it: an owned handle goes there as `borrow(fd)` or
`move(fd)`, a view as it is.

## Structs

```c
struct name { struct name * owned maybe next; char * owned text; };
struct tokenizer borrows(text) { const char * borrows(text) pos; int n; };
struct value { enum vkind k; union(k) { long i when(V_INT); char * view s when(V_STR); } u; };
struct regs { volatile uint32_t status w1c; volatile uint32_t id ro; volatile uint32_t tx wo; };
```

- `maybe` members are followed like variables: `if (q->next) q->next->v`.
- `union(k)`: a member is used only where `k` is known to select it.
- `ro` / `wo`: no write / no read; `w1c`: no read-modify-write that writes
  pending bits back.

## Methods

```c
struct stack
{
  int * owned items;
  int len, cap;
  int push(inout struct stack *self, int v) { /* ... */ return 0; }
  void destroy(drop struct stack * owned self) { /* ... */ }
};
s->push(10);  st.push(1);  stack.push(&st, 2);   /* all call stack__push */
```

## Slices

```c
int sum([]const int s)                 /* a pointer and its length, in r0:r1 */
{
  int t = 0;
  for (size_t i = 0; i < s.len; i++)
    t += s[i];                         /* checked against s.len: a trap past it */
  return t;
}

int a[16];
[]int all = a;                         /* from an array: len 16 */
[]int mid = a[4:8];                    /* slicing: a[x:y], a[x:], a[:y], a[:] */
[]int some = p[0:n];                   /* from a pointer: its end said */
[:0]const char name = "abc";           /* a literal: len 3, name[3] == 0 */
[:0]char path = p[0:n:0];              /* a C string: p[n] must be a zero (checked) */
path[end] = '\0';                      /* end <= path.len: its own zero too */
[][]const int rows = row_array;        /* slices of slices */
f(s.ptr, s.len);                       /* to regular C */
```

`s[i]` and slicing are checked at every `-O` (`udf #254` past the end, or for
`x[a:b]` with `a > b`), and the compiler drops each check it proves: the loop
above has none.  `.ptr` and `.len` are read-only: a slice changes as a whole.
A `[:0]T` keeps its zero: `s[s.len]` is read as 0, and written only with a
zero -- a non-zero there traps, below `len` anything goes (a zero just ends
the C string early; the slice keeps its `len`).  `x[a:b:0]` makes one,
checking `x[b] == 0` (and, but for a `[:0]` base, `b < len`).
A slice converts to `[]const T`, a `[:0]T` to `[]T`; nothing converts to a
slice but an array, a slice or slicing, and a slice to nothing else (use
`.ptr` / `.len`).  A slice parameter is a view: a view result may borrow from
it.  Regular C sees an `unsigned long long` (ptr in the low word).

## Parameters that say their buffer: checked at every call

```c
char *dirname(inout char path[maybe :0]);     /* a C string: its NUL is in the buffer */
void fill(inout char d[n], size_t n);         /* n elements of d, read and written */
ssize_t write(int fd, const void buf[n], size_t n);  /* const: read; void: bytes */
ssize_t read(int fd, out void buf[n], size_t n);     /* out: written */
void fill_first(size_t n, char d[n]);         /* the bound before or after the array */
```

Still a pointer (a view; `maybe` in the brackets when null is allowed), as C
passes it; regular C sees `char *`.  A call is checked where the caller names
the buffer (`__builtin_object_size`): a string with no NUL inside it ends the
program (`__chk_fail`), a size past it traps (`udf #254`); constants that fit
cost nothing, ones that do not warn too.  A buffer the compiler cannot see (a
pointer parameter) is not checked.  Inside the function `-fsanitize=bounds`
checks `d[i]` against `n`, also when `n` comes after `d`.  It is gcc's
`access(mode, ref, size)` and `null_terminated_string_arg(n)`, which regular
C headers write (docs/checks.md, fortify); `-fno-fortify` turns the call
checks off.

## Errors as values

```c
error ParseError { Empty, BadDigit, TooBig };
!int parse_int(const char *s);                /* an int or an error */

int a = parse_int(s) catch -1;               /* a default */
int b = parse_int(s) catch |e| { printf("%s\n", error_name(e)); return; };
return try parse_int(s);                     /* pass it up (enclosing fn returns !T) */
return error.Empty;  return error.ENOMEM;    /* errno names too (with <errno.h>) */
```

## Cleanup

```c
defer mem_free(buf);       /* on every way out of the scope, last first */
errdefer mem_free(buf);    /* only when the function returns an error */
```

## Unreachable

```c
int half(int x) { if (x % 2 == 0) return x / 2; unreachable; }  /* no made-up return */
switch (c) { case RED: ...; case GREEN: ...; default: unreachable; }
int fd = open_config() catch unreachable;    /* cannot fail here */
```

A point control never reaches.  Reached anyway, it traps (`udf #254`, as the
`-fsanitize` checks): the branch into it is the check, kept at every `-O`.
Code after it is dead, so a non-void function may end in one.
`default: unreachable` still gets `-Wswitch` for the enum's missing cases.
With `-fsanitize=unreachable -fno-sanitize-trap` it prints `file:line:
runtime error: execution reached an unreachable program point` and aborts.
`__builtin_unreachable()` is the unchecked one (an assumption, no code).

A path that falls off the end of a non-void function is reported
(`function might return no value`): an error in the safe default.  A loop
with no exit, a `noreturn` call, `__builtin_trap()` and `unreachable` end a
path.

## Casts: what may change

A C cast `(T)x` is an error in yasc (`-Wold-style-cast`, which names the cast
to write).  `cast<...>(x)` lists what may change; anything else is an error.
One permission carries the type; `align`, `qual` and `const` add to a pointer cast.

```c
uint16_t port = cast<int(uint16_t)>(atoi(a));          /* the value must fit: a trap if not */
uint8_t  lo   = cast<trunc(uint8_t)>(w >> 8);          /* the low bits, on purpose */
uint32_t bits = cast<bits(uint32_t)>(f);               /* the same bytes, the same size */
float    avg  = cast<float(float)>(sum) / n;           /* rounding allowed */
enum op  o    = cast<enum(enum op)>(byte);             /* must be an enumerator */
long long w   = cast<as(long long)>(x);                /* lossless only */
auto de = cast<ptr(struct dirent *), align>(buf + pos); /* pointee changes, alignment checked */
uintptr_t a   = cast<addr(uintptr_t)>(p);
char *hit = unsafe_cast<qual(char *)>(s);               /* drops const: strchr */
char *err = unsafe_cast<ptr(char *), const>("");        /* a const object as char *: dlerror */
volatile uint32_t *reg = unsafe_cast<addr(volatile uint32_t *)>(0x40070000u);
(void)unused;                                          /* stays */
```

| Permission | Allows | Check |
|---|---|---|
| `as(T)` | nothing lost (widening, adding const) | compile time |
| `int(T)` | to an integer, keeping the value (also from a float, toward zero) | the value fits, NaN traps |
| `trunc(T)` | integer to integer, the low bits | none |
| `bits(T)` | the bytes as T, the same size | the sizes |
| `float(T)` | to a floating type, rounding | none |
| `enum(E)` | integer to enum | an enumerator |
| `ptr(T *)` | the pointee type | the alignment may not rise unproven |
| `align`, `align(N)` | the alignment rises to N (`align`: the `ptr` type's) | proven, or a runtime test |
| `addr(T)` | pointer to integer; integer to pointer (`unsafe_cast`) | the integer is wide enough |
| `qual(T *)`, `qual` | drops const/volatile (`unsafe_cast`) | not from a const/volatile object |
| `const(T *)`, `const` | drops only const, from a const object too (`unsafe_cast`) | none: the object stays const |
| `fn(T)` | another function pointer type (`unsafe_cast`) | none |

The outermost `*` of the cast's type may say nothing: it takes the operand's
`owned`/`view`/`maybe`/`borrows`.  A cast is checked where the types do not
prove it (a constant at compile time; the optimizer drops what it proves);
`-fno-sanitize=cast` drops the runtime checks.  A cast in a regular-C
header's macro is regular C.  Details: `docs/yasc_casts.md`.

## Builtins: @min, @max

```c
size_t n = @min(buflen, PATH_MAX);  /* each operand once, left to right */
int v = @max(lo, @min(x, hi));      /* clamp */
float f = @min(a, b);               /* vminnm.f32 on FPv5; a NaN loses to a number */
```

The result has the usual arithmetic type.  A signed operand that may be
negative next to an unsigned one is an error (`a < b ? a : b` would turn -1
into UINT_MAX); a non-negative constant is fine.  Integers compile to
cmp/it/mov, `float` to one `vminnm`/`vmaxnm.f32` (RP2350, FPv5), `double` to
`fmin`/`fmax`; two constants fold.  Plain C files get them too.

gcc has no `@` token and no `#define` can make one, so code that a regular
compiler also builds spells it through a macro:

```c
#ifdef __TINYC__
#define MIN_OF(a, b) @min(a, b)
#else
#define MIN_OF(a, b) ({ __typeof__(a) _a = (a); __typeof__(b) _b = (b); _a < _b ? _a : _b; })
#endif
```

## Modules

```c
module lib.vec;                                  /* this file is a module */
export vec2_t vec_add(vec2_t a, vec2_t b);       /* private unless exported */
import lib.vec;                                  /* instead of #include */
```

## Regular C from yasc

| Situation | Rule |
|---|---|
| a pointer declared in regular C | `unchecked`: copies, compares, goes back to C freely |
| unchecked into `owned` / `view` | only with `unsafe(expr)` |
| owned into a regular-C parameter | `borrow(c)` (lend) or `move(c)` (hand over); an int handle too |
| `__attribute__((malloc))` | the result is `owned maybe` |
| `malloc(dealloc, n)` | `dealloc`'s n-th argument is `owned maybe drop` |
| `returns_nonnull` | with `malloc`: `owned`, not `maybe` |
| `nonnull(i)` | null there is `-Wnonnull` |

`unsafe { ... }` or `unsafe(expr)` is the only way to lower a lifetime error;
`-Wno-...` and `#pragma diagnostic` do not.

## What the checks report

| Mistake | Message (abridged) |
|---|---|
| no free on some path | `'s' leaks` |
| free twice / use after free | `'s' is freed twice` / `is used after it was freed` |
| `maybe` used untested | `'s' may be null where it is dereferenced` |
| `maybe` into a non-`maybe` | `a pointer that may be null is passed as argument 1` |
| owned result ignored | `an owned result is discarded` |
| a view kept | `'s' is a view: it cannot leave the function` |
| a view parameter freed or kept | `the parameter 'p' owns nothing (a view unless it says owned): write '* owned p' ...` |
| a handle in a plain int | `a handle is stored in a plain int: where it goes says 'int view' or 'int owned', or cast<as(int)>(fd) takes its number` |
| `view` on a parameter | `a parameter's pointer is a view unless it says owned: drop 'view'` (-Wredundant-view) |
| `view maybe` on a local | `a local's pointer is a view that may be null unless the function owns it: drop 'view'` (-Wredundant-view) |
| a plain local freed or given `malloc(n)` | `plain pointer in yasc: 'p' owns what it points at, say owned` (the note says why, fix-it `* owned`) |
| a borrow outliving its owner | `'tail' is used after 's', which it borrows from, was freed` |
| `out` read first | `'*n' is read before it is written` |
| an `in` parameter written | `assignment of read-only location` / `discards 'const'` |
| header and definition disagree | `'f' is declared again with another yasc contract: parameter 1 ('p') differs in ...` |
| owned moved silently | `ownership of 'p' moves here: write move(p)` |
| `NULL` or `0` as a pointer | `NULL is regular C: write nullptr` / `0 as a null pointer: write nullptr` |
| two pointers into a `restrict` argument | `argument 1 of 'cpy' is restrict, but argument 2 points into 'p' too` |

Apart from `-Wnull-dereference`, every check gives the same answer at every
`-O` and none changes the generated code.
