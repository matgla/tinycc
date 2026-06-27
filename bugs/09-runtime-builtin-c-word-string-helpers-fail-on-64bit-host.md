# 09 — `lib/builtin.c` word-at-a-time string helpers fail on 64-bit hosts

## Summary

The host-fallback implementations of `__tcc_strlen`, `__tcc_strcpy`, and
`__tcc_strcmp` in `lib/builtin.c` assume `sizeof(unsigned long) == 4`.  When
`lib/builtin.c` is compiled on a 64-bit host, the null-byte detection masks
(`0x01010101UL`, `0x80808080UL`) only inspect the low 32 bits of each loaded
word, causing the functions to read past the string terminator and return
incorrect results.

## Affected code

`lib/builtin.c`, inside the `#if !defined(__arm__)` blocks for:

- `__tcc_strlen`
- `__tcc_strcpy`
- `__tcc_strcmp`

## How to reproduce

Compile `lib/builtin.c` on an `x86_64` host and call `__tcc_strlen("hello")`:

```c
#define __ARM_EABI__ 1
#include "lib/builtin.c"

int main(void) {
    return __tcc_strlen("hello") != 5;
}
```

On a 64-bit host this returns a value larger than 5 (the exact value depends on
adjacent memory contents).

## Root cause

The classic word-at-a-time null-byte test

```c
(w - 0x01010101UL) & ~w & 0x80808080UL
```

works for 32-bit words.  On a 64-bit host `unsigned long` is 64 bits, but the
mask constants are still 32-bit, so the high 32 bits are never checked for a
null byte and the loop may overrun.

## Impact

- **ARM target**: none.  The runtime library is normally built for ARM, where
  `unsigned long` is 32 bits and the assembly versions in `lib/arm_string.S`
  are preferred.
- **Host testing / portability**: prevents host-native tests from covering these
  helpers on 64-bit Linux/macOS.  The new `tests/runtime/host/test_builtin_host.c`
  skips them on 64-bit hosts.

## Suggested fix

Use fixed-width types and constants for the word-at-a-time paths, e.g.
`uint32_t` with `0x01010101U` / `0x80808080U`, or expand the idiom to 64 bits
with `0x0101010101010101ULL` / `0x8080808080808080ULL` when
`sizeof(unsigned long) == 8`.

## Status

**FIXED** — `lib/builtin.c` now computes the word-at-a-time masks from
`sizeof(unsigned long)`, so the null-byte scan works on both 32-bit and 64-bit
hosts.  The host test in `tests/runtime/host/test_builtin_host.c` covers the
helpers on 64-bit Linux.
