# yasc: a tutorial

yasc is the C dialect of YasOS's tcc. It is C, plus a few words that say
who owns memory, and checks that hold you to it. It also has errors as
values, `defer`, and modules. It reads C headers as they are, so libc and
every C library stay usable.

Each chapter has a program next to this file. The test suite builds and
runs them all (`tests/ir_tests/test_qemu.py::test_yasc_tutorial`). It
requires them to compile with no warning and to print exactly their
`.expect` file. Chapter 3's diagnostics are tested too
(`test_yasc_examples.py`).

| Chapter | File | What it shows |
|---|---|---|
| 1 | `01_hello.c` | yasc is the default; every pointer says what it is |
| 2 | `02_ownership.c` | `owned`, `view`, `maybe`, `out`, `drop` |
| 3 | `03_mistakes.c` | the mistakes the checks catch (meant to warn) |
| 4 | `04_defer.c` | `defer` |
| 5 | `05_errors.c` | error sets, `!T`, `try`, `catch`, `errdefer` |
| 6 | `06_c_library.c` | using a C library: unchecked pointers, attributes |
| 7 | `07_methods.c` | methods: functions in a struct body |
| 8 | `../yasc_modules/` | modules: `module`, `export`, `import` |
| 9 | `09_config.c` | all of it together, with `#embed` and `#pragma tcc diagnostic` |
| | `mem.h`, `mem.c` | a small allocator in regular C, used by 2 to 5, 7 and 9 |
| | `09_settings.txt` | the settings chapter 9 builds in |
| | `counter.h`, `counter.c` | a small C library, used by 6 |

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
const char * view greeting = "hello, yasc";
```

In yasc a pointer either **owns** its target (`owned`: it must be freed or
handed on, exactly once) or **borrows** it (`view`: it never frees). A
plain `char *` is an error, "plain pointer in yasc: say owned or view".
The qualifier goes after the `*`, so each level of a pointer to a pointer
says what it is: `char * owned * view argv`.

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

The five words:

| Word | Where | Meaning |
|---|---|---|
| `owned` | after `*` | this pointer owns its target: free it or hand it on, once |
| `view` | after `*` | this pointer borrows; it never frees |
| `maybe` | after `*` | it may be null; test it before you dereference it |
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
static int count(const struct name * view maybe list, out int * view chars)
```

and one that frees says `drop`:

```c
static void free_all(drop struct name * owned maybe list)
```

The program prints `blocks left: 0`: everything it allocated was freed,
and the checks agreed before it ran.

## 3. What the checks catch

`03_mistakes.c` has one mistake per function. `tcc -O1 -c 03_mistakes.c`
says (`03_mistakes.diag`):

```
03_mistakes.c:16: warning: 's' leaks: it still owns memory at the end of its scope
03_mistakes.c:22: warning: 's' is freed twice
03_mistakes.c:21: note: 's' was freed here
03_mistakes.c:29: warning: 's' is used after it was freed
03_mistakes.c:28: note: 's' was freed here
03_mistakes.c:35: warning: 's' may be null where it is dereferenced
03_mistakes.c:42: warning: an owned result is discarded: it is never freed
03_mistakes.c:47: warning: a pointer that owns nothing is stored in an owned pointer
03_mistakes.c:52: warning: '*n' is read before it is written
03_mistakes.c:60: warning: 'x' may be used uninitialized
03_mistakes.c:66: warning: 'local_address' returns the address of a local variable
03_mistakes.c:71: warning: null passed as argument 1, which is declared nonnull
03_mistakes.c:86: warning: 'q->next' may be null where it is dereferenced
03_mistakes.c:99: warning: a pointer that may be null is passed as argument 1 of 'length', which is not maybe
03_mistakes.c:109: warning: 'tail' is used after 's', which it borrows from, was freed
03_mistakes.c:108: note: 's' was freed here
03_mistakes.c:115: warning: the address of a local variable is stored through 'slot', which outlives 'dangling'
03_mistakes.c:125: warning: a null pointer is dereferenced
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
  alone is not. Calling a function that may change `*q` (its parameter is not
  `const`) or assigning `q` again means testing `q->next` again.
- **Owned result discarded:** keep what an owned function returns, or free
  it.
- **A view into an owned parameter:** only an owner may free.
  `mem_free(borrowed)` would free someone else's memory.
- **`out` read before written:** an `out` parameter is for the callee to
  fill in, not to read.
- **Uninitialized, address of a local:** the C classics, caught on every
  path.
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
  address 0. This one depends on the optimizer, so it is reported at `-O1`
  and up (that is why this chapter is compiled with `-O1`); all the others
  are reported the same way at every level.

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

static !int parse_int(const char * view s)
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

`errdefer` is a `defer` that runs only when the function fails. A
function that builds something then gives it to the caller on success,
and frees it on every error path:

```c
static !char * owned make_label(const char * view num)
{
  int v = try parse_int(num);
  char * owned maybe buf = mem_new(32);
  if (!buf)
    return error.ENOMEM;  /* errno names are errors too */
  errdefer mem_free(buf);
  if (v == 0)
    return error.Empty;   /* buf is freed */
  snprintf(buf, 32, "item-%d", v);
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
it is. Two rules decide how its pointers behave.

**A plain C pointer is unchecked.** C says nothing about ownership, so
yasc takes your word for it. An unchecked pointer converts to `owned`,
`view` or `maybe` without a diagnostic, and the checks start at the yasc
variable it lands in. Pointers written in a C macro (`COUNTER_NONE`,
`NULL`) are unchecked too. A C function's parameter borrows: passing it
an owned pointer leaves that pointer owned.

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
test it for null, and give it to `counter_free`. Forgetting the free is a
leak warning. Freeing a view, or freeing it twice, is caught too.
`counter_name(c)`'s result has no attribute: it is unchecked, so a view is
fine.

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

  int push(struct stack * view self, int v) { ... }
  int empty(const struct stack * view self) { return self->len == 0; }
  void destroy(drop struct stack * owned self) { ... }
  struct stack * owned maybe create(int cap) { ... }
};
```

The object is the first parameter, and you write it out, as in Zig. Its
type says what the method does with the object, with the words of
chapter 2: a `view` self looks or changes fields, a `drop ... owned` self
consumes it. Call a method on an object or through a pointer:

```c
s->push(10);        /* stack__push(s, 10) */
st.empty();         /* stack__empty(&st) */
s->destroy();       /* s is moved: using it again is an error in the checks */
```

`obj.m()` passes `&obj`, `p->m()` passes `p` itself, so `owned` and
`maybe` on `p` count exactly as they would for any call. A self taken by
value (`struct range self`) gets a copy.

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
struct vec { int x, y; int area(const struct vec * view self); };
int vec__area(const struct vec * view self) { return self->x * self->y; }
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
!int add(struct config * view self, const char * view s, int n)
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
`main` gives the list a `defer cfg.clear();` in a block of its own, so the
entries are freed before the program prints `blocks left: 0`.

**`#pragma tcc diagnostic`** changes the warnings for part of a file:

```c
#pragma tcc diagnostic push
#pragma tcc diagnostic ignored "-Wignored-attributes"
static void say(const char * view fmt, ...) __attribute__((format(printf, 1, 2)));
#pragma tcc diagnostic pop
```

tcc does not check printf formats yet, and says so about a `format`
attribute. The attribute stays for compilers that do check, and the warning
is off for that declaration only. `ignored`, `warning` and `error` take any
`-W` option; `GCC` and `clang` work in place of `tcc`, and so does
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

## Cheat sheet

```
T * owned p          owns *p: free it or hand it on, once
T * view p           borrows *p: never frees
T * owned maybe p    may be null: test it before *p
out T * view p       the callee writes *p
drop T * owned p     the callee consumes p
defer STMT;          run STMT when the scope is left
!T f(void)           a T or an error
return error.NAME;   fail
try f()              pass f's error up
f() catch X          f's value, or X if it failed (also: catch |e| ...)
errdefer STMT;       run STMT only if the function fails
struct s { ... int m(struct s * view self); };   a method: s__m
obj.m(x)  p->m(x)    call it: s__m(&obj, x), s__m(p, x)
s.f(x)               a method without self (or any method, by name)
module a.b;          this file is module a.b
export DECL          visible to importers
import a.b;          use module a.b
#embed "f" suffix(, 0)            a file's bytes, at compile time
#pragma tcc diagnostic ignored "-Wx"   turn a warning off (push / pop)
tcc -fsyntax-only f.c             run the checks, build nothing
#pragma yasc         this file is yasc (under -ccompat, or in a header)
tcc -ccompat         compile regular C
```

More: `docs/static_analysis_todo.md` (what the checks cover),
`docs/plans/yasc_errors.md` (error unions), `docs/yasc_methods.md`
(methods), `docs/yasc_modules.md` (modules and legacy C).
