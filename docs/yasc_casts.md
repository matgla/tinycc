# yasc casts: one word per intent

**Status: implemented (2026-11-26)**, as accepted after two rounds.
`frontend/gen/type/yasc_cast.c` holds the parser and the rules,
`op/ubsan.c` (`yasc_cast_check_*`) the runtime checks. The tests are
`tests/ir_tests/test_yasc_casts.py` and `yasc_cast_cases.c`. What differs
from the proposal, and what is not done yet, is under
[Implementation notes](#implementation-notes).

Decided in the first round:
- a single form, `cast<...>`, with no `cast_X` short forms;
- the type is written inside the permission that sets it;
- the unsafe permissions need `unsafe_cast`;
- the C cast is an error in yasc from the start;
- `qual` gets static checks.

Added later (2026-11-26): the `const` modifier, for a const object handed out
through a `char *` that POSIX fixed before `const` existed (`dlerror`,
`strerror`, `getenv`). See [`const`: a const object behind `char *`](#const-a-const-object-behind-char-).

## Why

A C cast `(T)x` does whatever it takes to turn `x` into a `T`: widen,
truncate, reinterpret bits, change the pointee type, raise the alignment,
drop `const`, turn an integer into an address. One spelling hides up to three
of these at once, so neither the reader nor the compiler knows which one was
meant:

```c
len = getdents(dir->fd, (struct dirent *view)dir->buf, sizeof(dir->buf));
/* pointee char -> struct dirent, alignment 1 -> 4, and a view: which ones were meant? */
```

yasc replaces the C cast with a cast that lists what it allows. Any other
change is a compile error. A checked cast costs nothing where the compiler
can prove it holds. Elsewhere it traps. `@` stays reserved for compiler
builtins (`@min`, `@max`).

## The form

```c
cast<PERMISSION(TYPE), MODIFIER, ...>(EXPR)
unsafe_cast<PERMISSION(TYPE), MODIFIER, ...>(EXPR)
```

```c
uint16_t port = cast<int(uint16_t)>(n);
auto de = cast<ptr(struct dirent *), align>(dir->buf + dir->buf_pos);
char *view hit = unsafe_cast<qual(char *)>(s);
```

- **The type.** At most one permission carries the target type in
  parentheses. With none, the type stays as it is: `cast<align(4)>(p)`.
- **Modifiers.** `align`, `align(N)`, `qual` and `const` are permissions
  added to a pointer cast; they take no type (`qual(T *)` and `const(T *)`
  carry it when nothing else does).
- **The result** is an rvalue, like a C cast. The operand is an
  assignment-expression in parentheses.
- **No stale modifiers.** A modifier must be needed. `align` on an operand
  that is already aligned enough, or a `qual` or `const` that drops nothing, is an error
  ("align is not needed: the operand is already 4-aligned"). A cast that
  outlived its reason is found this way. A typed permission that changes
  nothing is fine: `cast<int(size_t)>(n)` stays portable when `size_t` is
  already `n`'s type.

## Permissions

### Values (one per cast, no modifiers)

| Permission | What may change | Checked |
|---|---|---|
| `as(T)` | nothing is lost: every source value is exactly a `T` (widening, `int32_t` to `double`, adding `const`) | at compile time; anything lossy is an error |
| `int(T)` | to an integer type, keeping the value: narrowing, sign change, from a float (toward zero), from an enum, from `bool` | the value must fit; NaN traps |
| `trunc(T)` | integer to integer, keeping the low N bits (the value mod 2^N, any signedness) | never: losing bits is the point |
| `bits(T)` | reinterpret the bytes, same size (`float` to `uint32_t`, a struct to `uint64_t`, vectors) | sizes must match (compile time) |
| `float(T)` | to a floating type, rounding allowed (`uint32_t` to `float`, `double` to `float`) | never |
| `enum(T)` | integer to enum | the value must be an enumerator; for an `__attribute__((flag_enum))` enum, any OR of its enumerators |

### Pointers (`ptr`/`addr`/`fn`, plus the modifiers)

| Permission | What may change | Checked |
|---|---|---|
| `ptr(T *)` | the pointee type | the alignment must not rise unless proven (otherwise add `align`); qualifiers may only be added (otherwise add `qual`) |
| `align`, `align(N)` | the known alignment rises to N (a power of two). `N` is a constant or a type (its alignment); bare `align` means `alignof(*T)` of the `ptr` type | proven: free. Otherwise a runtime check, `tst p,#N-1` then trap |
| `addr(T)` | pointer to integer (`uintptr_t`, `intptr_t` or wider), or **integer to pointer** | integer to pointer is unsafe |
| `qual(T *)`, or `qual` beside `ptr` | drops `const` / `volatile` from the pointee, or adds `const` below a non-const level (`char **` to `const char **`) | unsafe; static checks below |
| `const(T *)`, or `const` beside `ptr` | drops `const`, and only `const`, from the pointee, from a const object too | unsafe; no object check (see below) |
| `fn(T)` | function pointer to another signature | unsafe |

**Mixing.** Value and pointer permissions never mix in one cast. A conversion
that needs both is two casts.

**`unsafe_cast`.**
- Needed exactly when a cast uses an unsafe permission: `qual`, `const`,
  `fn`, or `addr` from integer to pointer.
- `cast` with one of these is an error, and so is `unsafe_cast` without one.
- `unsafe(...)` and `unsafe { }` do not stand in for it. The keyword itself
  says it, so `grep unsafe_cast` finds every one.

**yasc qualifiers are not cast.**
- No cast changes `owned`, `view`, `borrows` or `?`. `move`, `borrow`, a
  null test and `unsafe(...)` do that, as today.
- When the target type spells none of these words, it takes them from the
  source: `cast<ptr(struct dirent *)>(v)` with `v` a view gives a
  `struct dirent *view`.
- If the target spells different ones, the cast is an error that names the
  right tool.

**Alignment.**
- What `align` must prove comes from the operand's place: an array variable's
  `_Alignas`, an array member at an aligned offset in its struct, or an
  earlier `cast<align(N)>` on the same value.
- The place tracking is the -Wcast-align place analysis already in
  `type/assign_check.c` (uncommitted).
- A `void *` source counts as aligned, as in C, where implicit `void *` to
  `T *` stays legal.

## What `qual` checks

`qual` lets a write reach memory through a pointer whose type said "read
only". That is undefined when the object itself is const. On our boards such
an object lives in flash (`.rodata` in the XIP image), where a store either
faults or is lost. A dropped `volatile` lets the optimizer merge or remove
MMIO accesses. Static checks in front of the cast make it safer than today's
`(char *)s`, so `qual` comes with them:

1. **A const object is never made writable.** If the operand points at a
   const object, `qual` is an error. "A const object" covers a string
   literal, the address of a `const` variable, and a `const` global or
   member, either directly or through a pointer the flow analysis follows to
   one.
2. **The result is not written where the analysis sees the object.** If the
   object's origin is unknown (a parameter), the result may be returned or
   passed on: this is `strchr` handing back its argument. If the same
   function also stores through it, `-Wlifetime` warns that a store goes
   through a `qual` cast.
3. **A real volatile object stays volatile.** Dropping `volatile` from a
   pointer into an object declared `volatile` (an MMIO register, a variable
   an ISR shares) is an error. Dropping it from a pointer that was only
   declared `volatile *` for an interface is allowed.
4. **Nested levels.** Adding `const` below a non-const level
   (`char **` to `const char **`) is a `qual` cast. A store of a
   `const char *` through the result is an error.

## `const`: a const object behind `char *`

Some interfaces return `char *` to memory the caller must not write. POSIX
kept the old signatures (`dlerror`, `strerror`, `getenv`, `strchr`), so the
type says "writable" while the object is const, often a string literal in
flash. `qual` refuses that by check 1, which is right for every other cast.

`const` says it outright: the object is const and stays const; only its type
loses the `const` for the interface.

```c
char *?borrows(static) dlerror(void) {
  return unsafe_cast<ptr(char *), const>("");     /* or unsafe_cast<const(char *)>("") */
}
```

- It drops `const` and nothing else. Dropping `volatile`, or adding `const`
  below a non-const level, still needs `qual`.
- Check 1 does not apply: that is the point of the word. Checks 2 and 4
  (stores through the result) apply as for `qual`, once they exist.
- `const` that drops no `const` is an error, and so is `const` with `qual`
  in one cast.
- `qual` refusing a const object names `const` in its message, so the
  choice is made where the cast is written.

There is no runtime check. A store-address test against the flash range on
every store through such a pointer would cost more than the bug it catches,
and checks 1 and 4 already cover the cases the compiler can see.

## Examples

```c
/* --- integers ---------------------------------------------------------- */
isalpha((unsigned char)c)            ->  isalpha(cast<trunc(unsigned char)>(c))  /* ctype: the byte, not the value */
out[i] = (uint8_t)(w >> 8);          ->  out[i] = cast<trunc(uint8_t)>(w >> 8);
h = (uint32_t)(x * 0x9e3779b97f4a7c15ull >> 32)
                                     ->  h = cast<trunc(uint32_t)>(x * 0x9e3779b97f4a7c15ull >> 32);
return (int)strlen(s);               ->  return cast<int(int)>(strlen(s));        /* traps past INT_MAX */
size_t n = (size_t)len;              ->  size_t n = cast<int(size_t)>(len);       /* traps on len < 0 */
uint16_t port = (uint16_t)atoi(a);   ->  uint16_t port = cast<int(uint16_t)>(atoi(a)); /* 70000 traps */
uint8_t k = (uint8_t)300;            ->  error: 300 is not a uint8_t
printf("%lld", (long long)x);        ->  printf("%lld", cast<as(long long)>(x));
int dig = (int)(c - '0');            ->  int dig = c - '0';                       /* no cast needed */

/* --- floats -------------------------------------------------------------- */
float avg = (float)sum / n;          ->  float avg = cast<float(float)>(sum) / n;
int ms = (int)(sec * 1000.0);        ->  int ms = cast<int(int)>(sec * 1000.0);  /* toward zero; NaN traps */
uint32_t b; memcpy(&b, &f, 4);       ->  uint32_t b = cast<bits(uint32_t)>(f);
union { double d; uint64_t u; } x;   ->  uint64_t u = cast<bits(uint64_t)>(d);
double d = (double)i32;              ->  double d = cast<as(double)>(i32);        /* or implicit */

/* --- enums --------------------------------------------------------------- */
enum op o = (enum op)buf[0];         ->  enum op o = cast<enum(enum op)>(buf[0]); /* a stray byte traps */
int v = (int)st;                     ->  int v = cast<int(int)>(st);
mode = (enum perm)(PERM_R | PERM_W); ->  mode = cast<enum(enum perm)>(PERM_R | PERM_W); /* flag_enum */

/* --- pointers ------------------------------------------------------------ */
(struct dirent *view)dir->buf        ->  cast<ptr(struct dirent *)>(dir->buf)     /* _Alignas buffer: free */
(struct dirent *view)(buf + pos)     ->  cast<ptr(struct dirent *), align>(buf + pos) /* runtime check */
(struct sockaddr *)&sin              ->  cast<ptr(struct sockaddr *)>(&sin)       /* alignment does not rise */
const uint32_t *w = (const uint32_t *)src;  /* src: const uint8_t *, known 4-aligned by the caller */
                                     ->  const uint32_t *w = cast<ptr(const uint32_t *), align>(src);
p = (uint8_t *)cast_to_aligned;      ->  p = cast<align(8)>(p);                   /* type kept, alignment proven or checked */
struct node *n = (struct node *)malloc(sizeof *n);
                                     ->  struct node *?owned n = malloc(sizeof *n);  /* void *: no cast */
const char *t = (const char *)s;     ->  const char *t = s;                       /* adding const: implicit */
if ((uintptr_t)p & 3)                ->  if (cast<addr(uintptr_t)>(p) & 3)

/* --- unsafe -------------------------------------------------------------- */
return (char *)s;           /* strchr */
                            ->  return unsafe_cast<qual(char *)>(s);
#define UART0 ((volatile uint32_t *)0x40070000u)
                            ->  #define UART0 unsafe_cast<addr(volatile uint32_t *)>(0x40070000u)
char *w = (char *)"lit";    ->  error: a string literal is not made writable (check 1)
p = (int *)volatile_reg;    ->  error: 'volatile_reg' is a volatile object (check 3)
const char **cc = (const char **)argv;
                            ->  const char **cc = unsafe_cast<qual(const char **)>(argv);
                                *cc = "x";   /* error: a 'const char *' stored through a qual cast (check 4) */
qsort(v, n, sizeof *v, (int (*)(const void *, const void *))cmp_u32);
                            ->  qsort(v, n, sizeof *v, unsafe_cast<fn(int (*)(const void *, const void *))>(cmp_u32));
const uint8_t *src; uint32_t *dst = (uint32_t *)src;   /* const dropped and alignment raised */
                            ->  uint32_t *dst = unsafe_cast<ptr(uint32_t *), qual, align>(src);

/* --- errors that name the fix --------------------------------------------- */
(uint16_t)x                 ->  error: C cast: write cast<int(uint16_t)>(x), or cast<trunc(uint16_t)>(x) to keep the low bits
(struct dirent *)(b + pos)  ->  error: C cast: write cast<ptr(struct dirent *), align>(...)
cast<ptr(uint32_t *)>(b)    ->  error: the alignment rises from 1 to 4: add align, or declare the buffer _Alignas(uint32_t)
cast<int>(x)                ->  error: 'int' needs its type: int(T)
cast<qual(char *)>(s)       ->  error: qual is unsafe: write unsafe_cast
unsafe_cast<int(int)>(n)    ->  error: nothing unsafe here: write cast
cast<ptr(T *), align>(p)    ->  error: align is not needed: 'p' is already 4-aligned

(void)unused;               /* stays as in C */
```

## Checks

- **Compile time.** A constant operand is checked when the program is
  compiled.
- **Runtime.**
  - The value checks (`int`, `enum`, `align`, float to int) belong to a new
    `-fsanitize=cast` group, on in the safe default.
  - Where VRP or the place analysis proves the condition, no code is
    emitted.
  - Where it cannot, the check is a compare and a trap: the same handler and
    `file:line` message as the other sanitizers (`-fno-sanitize-trap` for
    the printed form).
  - `-fno-sanitize=cast` drops the checks. The permission rules still apply.
- **Implicit conversions are not touched.** C's implicit narrowing stays
  under `-Wconversion`, whose message then names the cast to write.

## The C cast in yasc

- `(T)x` is an error in yasc from the start. The message spells the
  replacement, as in the examples above.
- `scripts/safe_mode_ratchet.py` gets a new floor once the existing files are
  converted.
- What stays allowed:
  - `(void)x`;
  - `-ccompat` files and `#pragma tcc c_begin` sections;
  - casts written in system headers, including in macros defined there
    (`MAP_FAILED`, `SIG_IGN`);
  - the C headers that `-yasc-c-header` generates, where every `cast<...>`
    is written out as a C cast.

## Syntax details

- **Keywords.** `cast` and `unsafe_cast` are keywords in yasc only. Under
  `-ccompat` they are identifiers. The permission names (`as`, `int`,
  `trunc`, `bits`, `float`, `enum`, `ptr`, `align`, `addr`, `qual`, `fn`)
  mean something only inside `<` `>`, so they need no new keywords. `const`
  is a C keyword already; inside `<` `>` it is the modifier.
- **Inside the brackets.** The type is an abstract declarator, as in
  `sizeof(T)`. The `align(N)` argument is a constant expression or a type.
  Every argument is in parentheses, so a `>` or a `>>` inside them is never
  taken for the closing bracket.

## Implementation notes

- **Where the checks go.** The frontend emits no check for a conversion its
  types prove lossless, and checks a constant at compile time. The other
  value checks are always emitted: VRP and constant folding drop the ones
  they prove at `-O1` and up, and at `-O0` they stay.
- **`align`.** The proof comes from the operand's place: `_Alignas` on an
  array, an array member at an aligned offset, or a packed member's place.
  A `cast<align(N)>` result does not carry its alignment into a later cast.
  To change the pointee and the alignment, write both in one cast:
  `cast<ptr(T *), align(N)>`.
- **`qual` checks.**
  - Implemented: check 1 (an array whose elements are const, a string
    literal, `&x` of a const object) and check 3 (`&x` of a volatile
    object). Both see the operand only when it designates the object
    directly. An origin the flow analysis would have to follow is not
    checked yet.
  - Not yet implemented: check 2 (a store through the result in the same
    function) and check 4 (a `const char *` stored through a `char **` to
    `const char **` result). Both belong in the `-Wlifetime` flow.
- **`enum`.** The check is membership: a range test when the enumerators
  are contiguous, a 32-bit mask when they span fewer than 32 values, and
  otherwise one comparison per enumerator. `flag_enum` (Q5 of the first
  round) is not supported: tcc has no such attribute yet.
- **Regular-C macros.** The expansion of a macro defined in regular C that
  holds a cast, `(` followed by a type other than `void`, is read as
  regular C. Its arguments are read that way too, as already happens for a
  macro that holds a `*`.
- **The C cast.** `-Wold-style-cast` is an error in the safe default. In
  `#pragma yasc` files compiled with `-ccompat` it is off unless asked for.
  `-Wno-error=old-style-cast` turns it into a warning, which helps while
  files are being converted. Once it has been reported, the older cast
  checks (`-Wcast-align`, `-Wcast-qual`, the yasc "drops const") are not
  repeated for that cast.
- **yasc words in the type.** Only the outermost `*` of a cast's type may
  spell none of the words. A plain inner level is an error, as in a
  declaration. A target that spells different words gets the explicit-cast
  diagnostics ("a cast makes an owned pointer a view: write borrow(...)"),
  which `unsafe(...)` lowers.
- **Messages.** Typedef names are not kept by tcc, so a message spells the
  underlying type (`cast<int(unsigned short)>` for `uint16_t`).

## Order of work (as planned)

1. Parser, permission rules, compile-time value checks, the C-cast error with
   its replacement, and the `qual` checks 1, 3 and 4. Tests in
   `tests/ir_tests/test_yasc_casts.py` at every `-O`, plus a code-identity
   test (a proven cast emits nothing).
2. `-fsanitize=cast` runtime checks; the `align` proof from the place
   analysis; `qual` check 2 in the `-Wlifetime` flow.
3. `docs/yasc_cheatsheet.md`, the VSCode/yasvi highlighting,
   `-yasc-c-header` output. The existing files are converted (by hand),
   then the ratchet gets its new floor.
