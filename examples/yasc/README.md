# yasc: a tutorial

yasc is the C dialect of YasOS's tcc. It is C, plus a few words that say
who owns memory, and checks that hold you to it. It also has errors as
values, `defer`, and modules. It reads C headers as they are, so libc and
every C library stay usable.

Each chapter has a program next to this file. The test suite builds and
runs them all (`tests/ir_tests/test_qemu.py::test_yasc_tutorial`, at every
`-O`). It requires them to compile with no warning and to print exactly
their `.expect` file. Chapter 3's diagnostics are tested too
(`test_yasc_examples.py`).

The whole dialect on one page: [`docs/yasc_cheatsheet.md`](../../docs/yasc_cheatsheet.md).

For the checks that work on plain C as well (formats, buffer overflows,
stack budgets, sanitizers, ...), see the guide
[`docs/checks.md`](../../docs/checks.md) and its examples in
[`../checks/`](../checks/README.md).

| Chapter | File | What it shows |
|---|---|---|
| 1 | `01_hello.c` | yasc is the default; every pointer says what it is |
| 2 | `02_ownership.c` | `owned`, `view`, `maybe`, `inout`, `out`, `drop` |
| 3 | `03_mistakes.c` | the mistakes the checks catch (meant to warn) |
| 4 | `04_defer.c` | `defer` |
| 5 | `05_errors.c` | error sets, `!T`, `try`, `catch`, `errdefer` |
| 6 | `06_c_library.c` | using a C library: unchecked pointers, attributes |
| 7 | `07_methods.c` | methods: functions in a struct body |
| 8 | `../yasc_modules/` | modules: `module`, `export`, `import` |
| 9 | `09_config.c` | all of it together, with `#embed` and `#pragma tcc diagnostic` |
| 10 | `10_arrays.c` | arrays that own their elements, loops over them, int handles |
| | `mem.h`, `mem.c` | a small allocator in regular C, used by 2 to 5, 7, 9 and 10 |
| | `09_settings.txt` | the settings chapter 9 builds in |
| | `counter.h`, `counter.c` | a small C library, used by 6 |
| | `slots.h`, `slots.c` | handles from a C file, declared in yasc, used by 3 and 10 |

Build them on YasOS with `make`, and run them with `make run`.

## 0. Turning it on

tcc compiles yasc unless you say otherwise:

```sh
tcc prog.c -o prog          # yasc, with its checks: the safe default
tcc -ccompat old.c -o old   # regular C, as before
```

Safe mode also turns on `-Wall` and `-Wlifetime` (the ownership checks),
and makes unsafe C (implicit declarations, int/pointer mixing) an error.
With `-ccompat`, a file can still opt in with `#pragma yasc` at its top.
The examples here do that, so they build either way.

The dialect belongs to a file. The main file is yasc, and so is any file
that says `#pragma yasc`. Every header you `#include` is regular C unless
it says so itself, which is why `<stdio.h>` keeps working. In regular C,
`owned`, `view` and the other new words are ordinary identifiers.

## 1. Every pointer says what it is

```c
const char *greeting = "hello, yasc";
```

In yasc a pointer either **owns** its target (`owned`: it must be freed or
handed on, exactly once) or **borrows** it (`view`: it never frees). A
plain `char *` is an error, "plain pointer in yasc: say owned or view".
The qualifier goes after the `*`, so each level of a pointer to a pointer
says what it is: `char * owned * view argv`.

Two pointers need no word. A parameter's own (the last `*` before its
name) is a view unless it says `owned`, since the least a function can do
with it -- look -- is the default. `int len(const char *s)` borrows `s`.
A local pointer variable, like `greeting`, is a view too unless its
function owns it (gives it a `malloc` result, frees it, hands it on as
owned); then the compiler says "'p' owns what it points at, say owned". A
local view may hold null: what it holds is what flows into it, and the
checks follow that. `T * view p` on a local says more -- never null -- but
`view` on a parameter, or `view maybe` on a local, says nothing new and
draws a warning (-Wredundant-view). Inner levels still say it: `main`'s
`const char * view *argv`.

A view parameter is read-only unless it says `inout` or `out`: `main`'s
`argv` cannot reassign `argv[i]`, and the `const` keeps the strings
read-only too. A local you only read through is `const T *p`.

A string literal, an array decaying to a pointer, and `&x` are views.

## 2. Ownership

`02_ownership.c` keeps a list of names. The list owns its nodes, and each
node owns its text:

```c
struct name
{
  struct name * owned maybe next; /* the rest of the list, or null */
  char * owned text;              /* never null once the node exists */
};
```

The six words:

| Word | Where | Meaning |
|---|---|---|
| `owned` | after `*` | this pointer owns its target: free it or hand it on, once |
| `view` | after `*` | this pointer borrows; it never frees |
| `maybe` | after `*` | it may be null; test it before you dereference it |
| `inout` | before the type | the callee may read and write `*p` |
| `out` | before the type | the callee writes `*p` before it returns, and does not read it first |
| `drop` | before the type | the callee consumes the pointer (frees it, or hands it on) |

Plain pointers without `maybe` are never null. `maybe` combines with
either kind: `struct name * owned maybe next`.

Handing an owned pointer on is a **move**, and the code says so with
`move(p)`:

```c
static struct name * owned maybe push(struct name * owned maybe head, char * owned text)
{
  struct name * owned maybe n = mem_new(sizeof *n);
  if (!n)
  {
    mem_free(text);   /* out of memory: text is freed here */
    return head;      /* and head goes back to the caller */
  }
  n->text = move(text);   /* moves into the node */
  n->next = move(head);
  return n;               /* moves to the caller */
}
```

Each of the following is a move: passing a pointer to an `owned`
parameter, storing it in an `owned` variable or member, or returning it
from a function whose result is `owned`. Where an owned variable or member
is handed on, write `move(p)`; without it the compiler warns "ownership of
'p' moves here: write move(p)" (`-Wno-implicit-move` turns that off). Two
moves need no `move`: freeing (an argument for a `drop` parameter, such as
`mem_free(text)`) and returning a whole local or parameter (`return n;`),
since the variable ends there anyway. A member or a global handed back does
need it: `return move(s->buf);`. After a move the variable owns nothing.
Using it again is an error in the flow checks, and so is reaching the end
of its scope while it still owns something.

`borrow(p)` is the opposite: a view of p's memory that p keeps owning, for
a borrow that lasts (`auto v = borrow(p);`). `auto q = p;` with p owned is
an error, since it does not say which of the two is meant. A name of that
spelling in scope (a variable or function called `move`) wins over both.

A function that only looks takes views:

```c
static int count(const struct name * maybe list, out int *chars)
```

A view parameter's target is read-only unless the parameter says `inout`
(or `out`): `T *p` there means `const T * view p`, and a function that
changes `*p` says so, `void rename(inout struct name *n)`.

and one that frees says `drop`:

```c
static void free_all(drop struct name * owned maybe list)
```

The program prints `blocks left: 0`: everything it allocated was freed,
and the checks agreed before it ran.

## 3. What the checks catch

`03_mistakes.c` has one mistake per function. In yasc the lifetime
mistakes are errors: the file does not compile, which is the point of it.
`tcc -O1 -fmax-errors=0 -c 03_mistakes.c` says (`03_mistakes.diag`):

```
03_mistakes.c:16: error: 's' leaks: it still owns memory at the end of its scope
03_mistakes.c:22: error: 's' is freed twice
03_mistakes.c:21: note: 's' was freed here
03_mistakes.c:29: error: 's' is used after it was freed
03_mistakes.c:28: note: 's' was freed here
03_mistakes.c:35: error: 's' may be null where it is dereferenced
03_mistakes.c:42: error: an owned result is discarded: it is never freed
03_mistakes.c:47: error: the parameter 'borrowed' owns nothing (a view unless it says owned): write '* owned borrowed' when the function takes it over, 'drop' before the type too when it frees it
03_mistakes.c:52: error: '*n' is read before it is written
03_mistakes.c:60: error: 'x' may be used uninitialized: yasc wants a value on every path
03_mistakes.c:66: error: 'local_address' returns the address of a local variable
03_mistakes.c:71: warning: null passed as argument 1, which is declared nonnull
03_mistakes.c:86: error: 'q->next' may be null where it is dereferenced
03_mistakes.c:99: error: a pointer that may be null is passed as argument 1 of 'length', which is not maybe
03_mistakes.c:109: error: 'tail' is used after 's', which it borrows from, was freed
03_mistakes.c:108: note: 's' was freed here
03_mistakes.c:115: error: the address of a local variable is stored through 'slot', which outlives 'dangling'
03_mistakes.c:125: warning: a null pointer is dereferenced
03_mistakes.c:140: error: 'a[i]' leaks: it may still own memory at return
03_mistakes.c:152: error: 'a[i]' is freed twice
03_mistakes.c:160: error: 'h' leaks: it still owns a handle at return
03_mistakes.c:166: error: a handle that may be invalid is passed as argument 1 of 'slot_read', which is not maybe
```

How to fix each:

- **Leak:** free the pointer, or hand it on, on every path out. `defer`
  (chapter 4) makes that easy.
- **Freed twice / used after it was freed:** after `mem_free(s)` (or any
  move), `s` owns nothing. Do not touch it again, or give it a new value
  first.
- **May be null:** test a `maybe` pointer before you dereference it. After
  `if (!s) return;`, the checks know `s` is set. A `maybe` struct member is
  followed the same way: `if (q->next) q->next->v` is fine, `q->next->v`
  alone is not. Calling a function that may change `*q` (its parameter says
  `inout`) or assigning `q` again means testing `q->next` again.
- **Owned result discarded:** keep what an owned function returns, or free
  it.
- **A view into an owned parameter:** only an owner may free.
  `mem_free(borrowed)` would free someone else's memory.
- **`out` read before written:** an `out` parameter is for the callee to
  fill in, not to read.
- **Uninitialized:** yasc enforces initialization. A local read where some
  path left it without a value is an error, the "may" case too: give it a
  value on every path, or at its declaration. Its address may be taken only
  once it has a value, unless it goes to an `out` parameter, which fills it.
  An array or a struct needs an initializer (`= {}`), or `= undefined` when
  the code fills it later (chapter 10).
- **Address of a local:** the C classic, caught on every path.
- **Null for a `nonnull` parameter:** see chapter 6.
- **A `maybe` member:** `q->next` is `owned maybe`, so `q->next->v` needs
  `if (q->next)` first, exactly like a `maybe` variable.
- **A `maybe` value for a parameter that is not `maybe`:** `length` says its
  argument is never null, so test `s` before the call (or make the
  parameter `maybe` and test it inside).
- **A borrow outliving its owner:** `tail = s + 1` owns nothing; it points
  into `s`. Once `s` is freed (or moved, or assigned again), `tail` is dead.
  Read through it before the free, or copy what you need.
- **A frame address that escapes** (`-Wdangling-pointer`): `x` dies when
  `dangling` returns, but `*slot` belongs to the caller and lives on. The
  same goes for storing `&x` in a global.
- **A null dereference the optimizer finds** (`-Wnull-dereference`): at
  `-O1` and up `nothing()` is inlined and `*nothing()` becomes a load from
  address 0. That finding depends on the optimizer, which is why this
  chapter is compiled with `-O1`. At `-O0` the flow check reports the same
  line differently: `nothing()` returns a `maybe` pointer, so "a pointer
  that may be null is dereferenced". All the other reports are the same at
  every level.
- **Owned array elements** (chapter 10): the first loop fills `a`, nothing
  frees it, so each element may still own its block at `return`. Two free
  loops over the same elements free each one twice.
- **Handles:** `h` is an `int` that owns a slot (chapter 10). Leaving
  without `slot_close(h)` leaks it, and `slot_read` wants a valid handle,
  so test `h < 0` first.

The checks follow every path through the function (`if`, loops, `goto`,
early `return`). Apart from `-Wnull-dereference` they give the same answers
at every `-O` level, and none of them changes the generated code.

To check a file without building it, `tcc -fsyntax-only prog.c` runs the
parse and every check and writes nothing.

## 4. defer

`defer STMT;` runs `STMT` whenever its scope is left: falling off the end,
`return`, `break`, `continue`, or `goto` out. Defers run last registered
first. Cleanup then sits next to what needs it:

```c
char * owned maybe buf = mem_strdup(s);
if (!buf)
  return -1;
defer mem_free(buf);      /* freed on every way out */
...
if (buf[0] == 'Q')
  return 1;               /* still freed */
```

`04_defer.c` also shows the order (`cleanup 2` runs before `cleanup 1`),
an early return that skips the defers not yet reached, and a defer in a
loop body, which runs at every iteration, `continue` included. A defer
body may not itself `return`, `break`, `continue` or `goto` out of the
defer.

## 5. Errors as values

`!T` is "a `T`, or an error". An error is a name: `error.Empty`. You can
group names into sets:

```c
error ParseError { Empty, BadDigit, TooBig };

static !int parse_int(const char *s)
{
  if (!*s)
    return error.Empty;   /* fail */
  ...
  return v;               /* succeed */
}
```

There are three ways to deal with a failure:

```c
int a = parse_int("42") catch -1;               /* a default */
int b = parse_int(s) catch |e| { ... return; }; /* a handler; e is the error */
return try parse_int(a) + try parse_int(b);     /* pass it up */
```

`try f()` returns `f`'s error from the enclosing function, which must
itself return `!T`. `catch` can take a value, an expression using `|e|`,
or a block. A block that ends without leaving (`return`, `continue`, ...)
works as a statement on its own: `f() catch |e| { report(e); };`.
`catch unreachable` says the call cannot fail here: if it does, the program
traps at that line.

`unreachable` is for any place that cannot be reached, instead of a return
value made up to quiet the compiler:

```c
static int days(enum month m)
{
  switch (m)
  {
  case FEB: return 28;
  case APR: case JUN: case SEP: case NOV: return 30;
  case JAN: case MAR: case MAY: case JUL: case AUG: case OCT: case DEC: return 31;
  default: unreachable;   /* -Wswitch still names a month with no case */
  }
}
```

`errdefer` is a `defer` that runs only when the function fails. A
function that builds something then gives it to the caller on success,
and frees it on every error path:

```c
static !char * owned make_label(const char *num)
{
  int v = try parse_int(num);
  char * owned maybe buf = mem_new(32);
  if (!buf)
    return error.ENOMEM;  /* errno names are errors too */
  errdefer mem_free(buf);
  if (v == 0)
    return error.Empty;   /* buf is freed */
  snprintf(borrow(buf), 32, "item-%d", v);
  return buf;             /* buf goes to the caller */
}
```

`error_name(e)` spells any error, its own names and errno's alike.
`error_from(errno_value)` turns an errno into an error. `<yasc.h>` has
`try_errno(call)` and `try_neg(call)`, which wrap a call that reports
failure through `errno` or a negative return.

To C, an error union is an `unsigned long long`: the value is in the low
word and the error in the high word. C code can call these functions too.

## 6. Using a C library

`counter.h` is a C header like any other, and the yasc code includes it as
it is. Three rules decide how its pointers behave.

**A plain C pointer is unchecked.** C says nothing about ownership, so
yasc does not guess. An unchecked pointer can be held, copied, compared
and handed back to C freely; it becomes an `owned`, `view` or `borrows`
pointer only where the code vouches for it, with `unsafe(...)`:
`const char * view name = unsafe(counter_name(borrow(c)));`. Without it that
is an error, "a pointer from regular C becomes a view here: write
unsafe(...)" (`-Wunchecked`). Null needs no vouching. Pointers written in
a C macro (`COUNTER_NONE`, `NULL`) are unchecked too. yasc code itself
spells the null pointer `nullptr`: a `NULL` or a `0` it writes as one is a
warning, "write nullptr" (`-Wzero-as-null-pointer-constant`).

**An owned pointer handed to C says what happens to it.** A C parameter
does not say whether it keeps the pointer, so the call does:
`counter_add(borrow(c), 1)` lends it (c is still owned and still yours to
free), `stash(move(c))` hands it over (c is gone; using or freeing it
afterwards is reported). A plain `counter_add(c, 1)` is a warning, "'c' is
owned and goes to a regular-C parameter: write borrow(c) or move(c)". A
parameter the header marks as the deallocator (`malloc(dealloc, n)`, below)
takes it without either.

**Attributes are believed.** A C header that says what it means is held to
it:

```c
void counter_free(counter_t *c);
counter_t *counter_new(const char *name) __attribute__((malloc, malloc(counter_free, 1), nonnull(1)));
```

| Attribute | yasc reads it as |
|---|---|
| `malloc` | the result is `owned maybe` |
| `malloc(dealloc, n)` | also: `dealloc`'s n-th argument is `owned maybe drop` |
| `returns_nonnull` | with `malloc`: the result is `owned`, not `maybe` |
| `nonnull(i, ...)` | a null constant there is a `-Wnonnull` warning (in `-Wall`) |

So `06_c_library.c` must store `counter_new`'s result in an owned pointer,
test it for null, lend it to the other functions with `borrow(c)`, and give
it to `counter_free`. Forgetting the free is a leak warning. Freeing a
view, or freeing it twice, is caught too.
`counter_name(borrow(c))`'s result has no attribute: it is unchecked, so
taking it as a view needs `unsafe(...)`.

YasOS's libc carries these attributes on `malloc`, `calloc`, `realloc`,
`strdup` (with `free`), `fopen` (with `fclose`), `opendir` (with
`closedir`) and `popen` (with `pclose`). It puts `nonnull` on the string
functions that read a whole string. With it, yasc code calls them
directly:

```c
char * owned maybe s = strdup(name);   /* the yasc way */
char * view t = strdup(name);          /* warns: never freed */
char * owned u = malloc(n);            /* warns: may be null */
```

The examples use `mem.h` instead, so they build with any C library.
`mem.h` is plain C that declares the same attributes.

A header written in yasc says `#pragma yasc` at its top, and its pointers
are checked like the main file's.

## 7. Methods

A function written in a struct body is a method:

```c
struct stack
{
  int * owned items;
  int len, cap;

  int push(inout struct stack *self, int v) { ... }
  int empty(const struct stack *self) { return self->len == 0; }
  void destroy(drop struct stack * owned self) { ... }
  struct stack * owned maybe create(int cap) { ... }
};
```

The object is the first parameter, and you write it out, as in Zig. Its
type says what the method does with the object, with the words of
chapter 2: a plain (view) self looks at the fields, an `inout` self
changes them, a `drop ... owned` self consumes it. Call a method on an object or through a pointer:

```c
s->push(10);        /* stack__push(s, 10) */
st.empty();         /* stack__empty(&st) */
s->destroy();       /* s is moved: using it again is an error in the checks */
```

`obj.m()` passes `&obj`, `p->m()` passes `p` itself, so `owned` and
`maybe` on `p` count exactly as they would for any call. A self taken by
value (`struct range self`) gets a copy.

The object's owned fields stay its own across an `inout` self, and the checks
hold both sides to that:

- **The method** may free an owned field, but must give it a new value (or
  null, if the field is `maybe`) before it returns: `'b->buf' is freed when
  'reset' returns: 'b' still owns it`.
- **The caller** hands the object over with its owned fields set and not
  freed (`'s.buf' is freed when its struct is handed to 'touch'`). After
  the call the fields still own memory, so `s.buf` must still be freed (a
  `maybe` one may now be null).
- **A `drop struct T *self`** method (a `clear` or `deinit`) frees
  the owned fields instead. It must free every one before it returns, and the
  caller's fields count as freed after the call, so `defer s.clear();` is
  enough to keep `s` from leaking.

```c
struct buf { char * owned maybe data; int len; };
void buf_fill(inout struct buf *b);    /* keeps data valid */
void buf_clear(drop struct buf *b);    /* frees data */

void f(void)
{
  struct buf b = {};
  defer buf_clear(&b);
  buf_fill(&b);
}
```

A method without a self is called through the struct's name:
`stack.create(4)`. That form works for every method:
`stack.empty(&st)`, and a typedef name works too.

A method takes no room in the struct. It is an ordinary function named
`TAG__NAME` (`stack__push`), which C code can call and link by that name.
`static` and `inline` mean what they mean for any function. Bodies are
compiled after the struct is complete, so they can use every field and
call every method, whatever the order. A method can also be declared in
the struct and defined outside it under its full name:

```c
struct vec { int x, y; int area(const struct vec *self); };
int vec__area(const struct vec *self) { return self->x * self->y; }
```

A struct with methods needs a tag, and must be defined at file scope (not
inside a function or another struct). A field and a method cannot share a
name. Methods with bodies belong in one file: a module (chapter 8) shares
them, with no header. In a header that several files include, write them
`static inline`.

## 8. Modules

A yasc file can be a module. Its declarations are private unless it
exports them, and other files `import` it instead of including a header:

```c
module lib.vec;
export typedef struct vec2 { int x, y; } vec2_t;
export vec2_t vec_add(vec2_t a, vec2_t b);
```

```c
import lib.vec;
```

Compiling a module writes its interface (`lib/vec.ymi`), and compiling the
importer reads it. `tcc -fmodule-deps` tells a build which order to use.
`-fmodule-header=DIR` writes a C header of the module for C callers.
`../yasc_modules/` has the full example and its walkthrough. The
reference is `man yasc-modules` and `docs/yasc_modules.md`.

Exporting a struct exports its methods with it: importers call them as
methods, and the C header lists them as plain functions (`stack__push`).
An `inline` method's body goes to importers too.

## 9. Putting it together

`09_config.c` reads settings: `09_settings.txt`, built into the program, is
parsed into a list that owns its entries. Most of it is chapters 2 to 7 at
work; three things are new.

**`#embed`** (C23) puts a file's bytes into an initializer at compile time.
`suffix(, 0)` appends a NUL, so the array is a string:

```c
static const char settings[] = {
#embed "09_settings.txt" suffix(, 0)
};
```

`prefix(...)`, `if_empty(...)` and `limit(n)` work too, and
`__has_embed("file")` tests for a file in `#if`.

**A method that fails** returns `!T` like any function. `add` makes the two
strings, then the entry, and `errdefer` frees what is already made when a
later step fails. On success they move into the entry:

```c
!int add(inout struct config *self, const char *s, int n)
{
  ...
  char * owned key = try copy(s, key_end);
  errdefer mem_free(key);    /* freed if a later step fails */
  char * owned value = try copy(s + v, n - v);
  errdefer mem_free(value);
  struct entry * owned maybe e = mem_new(sizeof *e);
  if (!e)
    return error.ENOMEM;     /* key and value are freed */
  e->key = move(key);        /* the entry owns them now */
  ...
}
```

The caller reports a bad line and goes on:
`self->add(p, n) catch |e| { say("line %d: %s\n", line, error_name(e)); };`.
`clear` takes a `drop struct config *self` (chapter 7): it frees every
entry, and `main` gives the list a `defer cfg.clear();` in a block of its
own, so the entries are freed before the program prints `blocks left: 0`.

**`#pragma tcc diagnostic`** changes the warnings for part of a file
(not the lifetime errors: see "unsafe" below):

```c
#pragma tcc diagnostic error "-Wformat"
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
```

The `format` attribute has tcc check every call of `say()` the way it checks
`printf`'s (`-Wformat`): `say("%d settings\n", "four")` would be "format
'%d' expects argument of type 'int', but argument 2 has type 'char *'".
The pragma makes that an error from its line to the end of the file,
whatever the command line says. `ignored`, `warning` and `error` take any
`-W` option; `push` and `pop` limit a change to the lines between them;
`GCC` and `clang` work in place of `tcc`, and so does
`_Pragma("tcc diagnostic ...")` in a macro.

The program prints:

```
line 5: NoEquals
line 6: EmptyKey
4 settings
name: yasc demo
area: 60
depth: BadNumber
speed: NotFound
blocks left: 0
```

## 10. Arrays and handles

`10_arrays.c` keeps owned pointers in arrays. The checks follow an array
element by element, and a counted loop over it as a whole:

```c
char * owned maybe copy[N] = undefined;
int i;
for (i = 0; i < N; i++)
  copy[i] = mem_strdup(words[i]);   /* every element owns a string (or is null) */
for (i = 0; i < N; i++)
  if (copy[i])                      /* each one is maybe: test it */
    ...
for (i = 0; i < N; i++)
  mem_free(copy[i]);                /* every element is freed: no leak */
```

yasc enforces initialization, and does not follow what is stored in an
array later: an array needs an initializer, or `= undefined`, which says the
code fills it before use and costs nothing. (`= {}` would make every element
a null pointer, owning nothing.)

A loop of the form `for (i = C0; i < N; i++)` (or `<=`, counting down, `i`
declared in the loop) whose range lies inside the array sets exactly the
elements it visits. Leave out the last loop and tcc says "'copy[i]' leaks:
it may still own memory"; free twice and it says "'copy[i]' is freed twice"
(chapter 3). Inside a loop, `copy[i]` is one element for as long as `i` does
not change: `mem_free(copy[i]); copy[i][0]` is a use after free. Members of
an array of structs (`lines[i].text`) work the same way. Any other loop (a
variable bound, a `break` out) leaves the elements "may be owned", and only
certain mistakes are reported for them. The limits are listed in
`docs/static_analysis_todo.md` ("What the flow checks do not follow").

**Handles** are ints that own something, as file descriptors do. `slots.h`
declares `slots.c`'s functions in yasc:

```c
int owned(slot_close) maybe slot_open(int which);  /* -1 when it fails */
int slot_close(int drop owned(slot_close) maybe h);
int slot_read(int view h);
```

`owned(slot_close)` names the function that releases the handle, and `maybe`
means it may be invalid. For an int, invalid means `< 0`, so `h < 0`,
`h == -1` and `h >= 0` are the tests (`if (h)` is not: 0 is a valid
handle). The rules are the pointer rules: close it on every path (`defer
slot_close(h);`), do not close it twice, test it before passing it to a
`view` parameter. An array of handles follows the same loops as an array of
pointers. YasOS's libc declares `open`, `close`, `read`, `write`, `socket`,
`pipe` and friends this way, so a file descriptor in yasc code is checked
like any other handle.

`owned(dealloc)` works for pointers too: `FILE * owned(fclose) maybe f =
fopen(...)`, and freeing it with `free` instead of `fclose` is an error.

The program prints:

```
total length: 19
open while reading: 4
sum: 406
first value: 107
lines: 4
blocks left: 0, handles left: 0
```

## unsafe: when the checks cannot see it

The lifetime errors (`-Wlifetime`, `-Wimplicit-move`, `-Wdangling-pointer`,
`-Wreturn-local-addr`) cannot be turned off in yasc: not with `-Wno-...`,
not with `#pragma tcc diagnostic`. Where the code is right and the checks
cannot follow it, say so in the code:

```c
unsafe {
  /* lifetime errors are not said here */
}
char * owned p = unsafe(adopt(raw));   /* one expression */
```

`unsafe` works by lines, so a region has its lines to itself: `unsafe {`
begins its line and its `}` ends it, and a statement with `unsafe(...)` is
alone on its line. Anything else there is an error. Regular C (`-ccompat`) has none of
these errors. `unsafe` is a keyword only in yasc, and a variable named
`unsafe` still wins.

## Unions and hardware registers

A union member may have been written as another member, so the checks do
not know what a yasc pointer in a union holds. A union with no owned /
view pointers in it is not checked at all; that is the usual register
type:

```c
typedef union {
  uint32_t raw;
  struct { uint32_t en:1, irq:1, _r:6, div:8, _r2:16; } b;
} ctrl_t;
```

A view member read through a union is a pointer from regular C: keep it,
compare it, pass it to C, but use it as a view only in `unsafe`. Storing
into it is checked as usual. An owned member in a union is an error.

```c
union val { char * view s; uintptr_t addr; };
int count(const char *s);

int len(union val *v)
{
  return count(unsafe(v->s));   /* without unsafe: an error */
}
```

When the struct already says which member is live, make it a tagged union
(like Zig's `union(enum)`, with the tag an ordinary member): each member says
which tag values select it, and a member is used only where the tag is known
to be one of them -- tested by `if`, `switch` or an early return, or just
stored. Then a view in it is a view, no `unsafe`:

```c
enum vkind { V_INT, V_STR };

struct value {
  enum vkind k;                       /* the tag: declared before the union */
  union(k) {
    long i          when(V_INT);
    char * view s   when(V_STR);
  } u;
};

int show(struct value *v)
{
  switch (v->k) {
  case V_INT: return printf("%ld\n", v->u.i);
  case V_STR: return printf("%s\n", v->u.s);
  }
  return printf("%s\n", v->u.s);   /* error: 'u.s' is used where 'k' may not be V_STR */
}

void set_int(inout struct value *v, long n)
{
  v->k = V_INT;          /* the tag first ... */
  v->u.i = n;            /* ... then the member */
}
```

A call that may write through `v` (an `inout struct value * view`
parameter) forgets what was known about `v->k`: test it again after it. A
`switch` on the tag with no `default` must handle every variant. A copy of
the tag works too (`enum vkind k = v->k; switch (k)`) while neither changes.

A variant may own memory. Then the variant changes only after that memory is
freed or moved out:

```c
struct name {
  enum { N_NONE, N_HEAP } k;
  union(k) {
    int none                 when(N_NONE);
    char * owned maybe heap  when(N_HEAP);
  } u;
};

void name_clear(inout struct name *n)
{
  if (n->k == N_HEAP)
    free(n->u.heap);     /* without it: 'u.heap' may still own memory when 'k' changes */
  n->k = N_NONE;
}
```

A bit-field store to volatile memory, `r->b.en = 1`, reads the whole
register and writes it back: a write-1-to-clear flag that was set is
cleared, a write-only register is read. yasc refuses it
(`-Wvolatile-bitfield`); write the read-modify-write out, so that it shows:

```c
ctrl_t v = *r;     /* one read */
v.b.en = 1;
v.b.irq = 0;       /* W1C: do not write back the pending flag */
*r = v;            /* one write */
```

Say what the hardware does with a field, after its declarator: `ro`
(read-only), `wo` (write-only) or `w1c` (write 1 to clear). Writing an `ro`
field or reading a `wo` one is an error, and so is writing a `w1c` field
back as it was read -- forgetting `v.b.irq = 0` above, or `status |= X` on a
status register, which clears every pending flag:

```c
typedef union {
  uint32_t raw;
  struct { uint32_t en:1, irq:1 w1c, _r:30; } b;
} ctrl_t;

struct uart {
  volatile uint32_t data wo;
  volatile uint32_t status w1c;
  volatile uint32_t id ro;
};

u->status |= RX_DONE;   /* error: w1c field 'status' is written back as it was read */
u->status = RX_DONE;    /* clears RX_DONE only */
```

## Cheat sheet

```
T * owned p          owns *p: free it or hand it on, once
T * view p           borrows *p: never frees
borrow(p)  move(p)   an owned p lent / handed over (also to a C function)
T * owned maybe p    may be null: test it before *p
inout T * view p     the callee may write *p (a view parameter's *p is read-only without it)
out T * view p       the callee writes *p
drop T * owned p     the callee consumes p
defer STMT;          run STMT when the scope is left
!T f(void)           a T or an error
return error.NAME;   fail
try f()              pass f's error up
f() catch X          f's value, or X if it failed (also: catch |e| ...)
errdefer STMT;       run STMT only if the function fails
struct s { ... int m(struct s *self); };   a method: s__m
obj.m(x)  p->m(x)    call it: s__m(&obj, x), s__m(p, x)
s.f(x)               a method without self (or any method, by name)
module a.b;          this file is module a.b
export DECL          visible to importers
import a.b;          use module a.b
#embed "f" suffix(, 0)            a file's bytes, at compile time
int owned(close) maybe fd           a handle: close it once; invalid when < 0
T * owned(fclose) maybe f          owned, released by fclose (not free)
#pragma tcc diagnostic ignored "-Wx"   turn a warning off (push / pop)
unsafe { ... }  unsafe(e)          the lifetime errors are not said there
unsafe(u.s)                        a view read from a union member
union(k) { T m when(K); ... } u;   a tagged union: m used only where k == K
ctrl_t v = *r; v.b.f = 1; *r = v;  a volatile bit-field store, written out
uint32_t f:1 w1c;  x ro;  y wo;     register fields: write 1 to clear, read-only, write-only
tcc -fsyntax-only f.c             run the checks, build nothing
#pragma yasc         this file is yasc (under -ccompat, or in a header)
tcc -ccompat         compile regular C
```

More: `docs/checks.md` (every check, how to turn it on and off),
`docs/static_analysis_todo.md` (what the checks cover, and their limits),
`docs/plans/yasc_errors.md` (error unions), `docs/yasc_methods.md`
(methods), `docs/yasc_modules.md` (modules and legacy C).
