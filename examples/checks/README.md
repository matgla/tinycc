# Catching bugs with tinycc: examples

Programs for [`docs/checks.md`](../../docs/checks.md), the guide to tcc's
checks. These are regular C, built with `-ccompat`: everything here works on
C code you already have. The yasc side (ownership, `maybe`, handles) is the
tutorial in [`../yasc/`](../yasc/README.md).

| Chapter | Files | What it shows |
|---|---|---|
| 1 | `01_find_the_bugs.c`, `.diag` | twenty bugs, one per function, and what tcc says about each (meant to warn) |
| | `01_fixed.c`, `.expect` | the same program fixed: clean with `-Werror`, and what it prints |
| 2 | `02_stack_budget.c`, `.diag`, `.stack`, `.expect` | frame and stack limits, `-Wvla`, `.su` files, the call-graph report of `scripts/stack_usage.py` |
| 3 | `03_sanitizers.c`, `.expect` | `-fsanitize=undefined`: trap mode, and handler mode with libc's reports |

The test suite checks all of it at `-O0` and `-O2`
(`tests/ir_tests/test_checks_examples.py`): each `.diag` is exactly what tcc
prints, each `.expect` exactly what the program prints on QEMU.

## Building

On YasOS (or with the cross compiler, `make TCC=armv8m-tcc`):

```sh
make            # build the programs
make bugs       # chapter 1: the warnings
make stack      # chapter 2: the warnings and the stack report (python3, on the host)
make run        # run them; ub traps on purpose, so it runs last
```

## 1. Find the bugs

```sh
tcc -ccompat -Wall -Wextra -Wuninitialized -Wmaybe-uninitialized \
    -Wdangling-pointer -Wnull-dereference -Wformat-security -c 01_find_the_bugs.c
```

Without `-ccompat`, tcc compiles yasc and turns these checks on by itself
(the safe default). A C file has to ask for them, and those options are the
safe default's checks by name. Every function in the file has one bug, and
each gets one warning (two for the `strcpy` and `sprintf` overflows, whose
note says what to write instead):

```
01_find_the_bugs.c:22: warning: format '%d' expects argument of type 'int', but argument 2 has type 'long'
01_find_the_bugs.c:28: warning: format not a string literal and no format arguments
01_find_the_bugs.c:34: warning: suggest parentheses around assignment used as truth value
01_find_the_bugs.c:46: warning: this statement may fall through
01_find_the_bugs.c:47: note: here
01_find_the_bugs.c:59: warning: enumeration value 'BLUE' not handled in switch
...
01_find_the_bugs.c:174: warning: a null pointer is dereferenced
```

`01_find_the_bugs.diag` has the whole list. `01_fixed.c` fixes each bug the
usual way, with a comment on what changed: `%ld`, `"%s"`, `==`, a
`/* fall through */` comment, the missing `case`, a cast after a sign test,
`snprintf`, a static where a local's address escaped, and so on. It builds
with the same options plus `-Werror`.

`say()` carries `__attribute__((format(printf, 1, 2)))`, so its calls are
checked the way `printf`'s are. Any function that takes a format can say
that. tcc checks a C library's `printf` only when its header carries the same
attribute (newlib's and glibc's do; YasOS libc's are being annotated).

## 2. A stack budget

A YasOS process stack is 4 to 8 KB. `02_stack_budget.c` has
`run_unbounded()`, with a 2 KB local buffer, a VLA and recursion, and
`run_bounded()`, which does the same work in a known amount of stack.

```sh
tcc -ccompat -O2 -Wframe-larger-than=1024 -Wstack-usage=1024 -Wvla \
    -fstack-usage -fcallgraph-info=su -c 02_stack_budget.c
```

```
02_stack_budget.c:30: warning: the frame size of 'checksum_big' is 2064 bytes, larger than 1024 bytes
02_stack_budget.c:30: warning: stack usage of 'checksum_big' is 2064 bytes, larger than 1024 bytes
02_stack_budget.c:35: warning: variable length array 'squares' is used
02_stack_budget.c:42: warning: stack usage of 'sum_squares' might be unbounded
```

`02_stack_budget.su` lists each function's frame, and `02_stack_budget.ci`
also lists its calls. The script adds up the frames along every path from
an entry point (`main` by default):

```sh
python3 scripts/stack_usage.py --libtcc1 --unknown-budget 256 \
    -e run_unbounded -e run_bounded 02_stack_budget.ci
```

```
run_unbounded: UNBOUNDED (recursion, dynamic); 2344 bytes on the deepest bounded path
         24      24  run_unbounded  [static]
       2064    2088  checksum_big  [static]
        256    2344  __tcc_memset  [budget]
run_bounded: 296 bytes
         24      24  run_bounded  [static]
         16      40  checksum_static  [static]
        256     296  __tcc_memset  [budget]
```

`--libtcc1` reads the compiler's runtime (`armv8m-libtcc1.ci`). Functions no
`.ci` describes, here the assembly `__tcc_memset` and libc's `printf`, cost
the `--unknown-budget`. `--limit 1024` makes the script exit 1 when an entry
point needs more than that: a check for CI. The byte counts are those of the
generated code, so they depend on `-O` (`-O0` frames are larger), and the
functions are `noinline` so that each one keeps its own frame.

## 3. Sanitizers

`03_sanitizers.c` does five things C leaves undefined: a load through null,
a signed overflow, a division by zero, a shift by 33 and an index past the
end. Without a sanitizer it prints wrong numbers and carries on.

```sh
tcc -ccompat -fsanitize=undefined 03_sanitizers.c -o ub
```

This is trap mode, the default: each check is a test plus `udf #254` in the
code, with no runtime library. The program stops at step 1. On the board
that is a fault; under a debugger (`ptrace`) it is a stop at the check.

```sh
tcc -ccompat -fsanitize=undefined -fsanitize-recover=undefined 03_sanitizers.c -o ub_report
```

This is handler mode: a failed check calls libc's `__ubsan_handle_*`, which
prints the file, the line and what went wrong on stderr, then lets the
program go on (`03_sanitizers.expect`):

```
step 1
03_sanitizers.c:55: runtime error: load of null pointer of type 'int'
load: read
step 2
03_sanitizers.c:23: runtime error: signed integer overflow: 2147483647 + 1 cannot be represented in type 'int'
add: -2147483648
...
```

With `-fno-sanitize-recover=undefined` the program reports the first failure
and aborts. Inputs come from a `volatile`, so nothing is folded at compile
time; a constant overflow is a compile-time warning (chapter 1) instead.
