# yasc: slices

A slice is a pointer and the number of elements it may reach, carried
together: `[]T`.  Decided 2026-11-24: Zig's slices in yasc's C syntax, always
checked, with the C ABI unchanged at the boundary.

## Syntax

| Write | Meaning |
|---|---|
| `[]T s` | a slice of T (the prefix goes before the type, as `!T`) |
| `[:0]T s` | a slice whose element at `len` is a zero (C strings) |
| `[]char *view argv` | a slice of `char *view` (pointer derivations bind to T, as for `!T`) |
| `[][]int rows` | a slice of slices |
| `s[i]` | the element, an lvalue; `i < s.len` checked |
| `x[a:b]`, `x[a:]`, `x[:b]`, `x[:]` | slicing an array, a slice or (with `b`) a pointer |
| `x[a:b:0]` | slicing to a `[:0]` slice: `x[b]` is checked to be a zero |
| `s.ptr`, `s.len` | the parts, read-only |

`a..b` (Zig) is not possible in C: `0..n` is one preprocessing number.  Go's
`a:b` is unambiguous inside `[]` (a `?:` is parsed whole before the `:` is
looked at).

## Representation and ABI

A 64-bit unsigned value, ptr in the low word, len in the high one, as `!T`
(docs/plans/yasc_errors.md): an argument or a result in r0:r1, a member of 8
bytes aligned 8, no memory and no copy for a slice in a register at -O0.
Regular C sees `unsigned long long`.  The type is an interned node
(`YASC_SLICE_NODE` in Sym.ymod) whose type is T and whose `c` is the
sentinel.

## Checks

- `s[i]`: `i <u len`; slicing: `b <=u len` (array, slice) and `a <=u b`.
  The failing side is `udf #254`, as -fsanitize's trap mode.
- A `[:0]T` holds its zero (decided 2026-11-24): `s[i]` is checked
  `i <=u len`, so the zero is read; a store into `s[i]` (`=`, `op=`, `++`,
  `--`) also traps when `i == len` and the value stored, converted to T, is
  not a zero.  A zero constant costs nothing more, a value not known one
  compare taken only at len.  No use of a `[:0]` slice takes its terminator
  away: below len a zero only ends the C string early (the slice keeps its
  len), and `[]T` from it does not reach len at all.
- ssa:vrp knows two facts for C strings (2026-11-24): `r = strnlen(p, m)`
  read at `p[r]` is a zero where `r != m` (or `r <u m`) held and nothing
  wrote memory since the call -- also through an inlined function's return
  slot, which SSA leaves in memory -- so `p[0:n:0]` after it costs nothing;
  and `i + 1 <=u len` for an i below len (dirname's `end`).  libgen has no
  check left at -O1 and up (its `path_len` is `inline always_inline`: the
  strnlen has to be in the function that slices).
- `x[a:b:0]`: `x[b] == 0`, read once; `b <u len` for an array or a `[]`
  slice, `b <=u len` for a `[:0]` slice (its own zero may end it), nothing
  for a pointer.
- On at every -O, in every yasc file.  The optimizer removes what it proves:
  ssa:vrp's induction-variable rules (counting up to `s.len`, counting down
  from it), constant indexes, and values the same through copies
  (`s.len` read twice is `s >> 32` twice).

## Conversions

- array of T -> `[]T` (len: the array's length; a string literal: its
  characters, without the zero) and `[:0]T` (literals only).
- `[]T` -> `[]const T`, `[:0]T` -> `[]T`.
- Nothing else: a pointer only by slicing with its end (`p[0:n]`), and a
  slice to nothing but a slice (`.ptr` and `.len` for regular C).
- Arithmetic, comparisons, casts and tests of a slice are errors.

## Ownership

A slice parameter is a view: the elision counts it among a view result's
sources and `borrows(s)` may name it.  A slice does not own; `owned` slices
(an allocation with its length) are not there yet.

## Not done yet

- -Wlifetime does not follow a slice of a frame array out of its function
  (`return local[0:2]` is not reported).
- `for (x : s)`, `owned` slices, slices of VLAs (`v[0:n]` works), a `[:0]`
  from a pointer whose length is not known (`cstr(p, max)`: `p[0:n:0]` takes
  n from strnlen).
- `while (s[i]) i++` over a `[:0]` keeps its `i <=u len` check: that the
  zero stops the loop by len is not something ssa:vrp knows.
- ssa:bounds_iv (the flat pass before the loop passes) does not see slices:
  their checks go in ssa:vrp, after unrolling and IV strength reduction.
