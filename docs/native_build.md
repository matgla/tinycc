# Building tinycc on YasOS

On the target, in a copy of this repository:

```sh
./configure            # add --enable-O0 for a faster build of a bigger compiler
make                   # armv8m-tcc, armv8m-libtcc1.a, lib/fp/*
./armv8m-tcc -v
```

`configure` sees `uname` = YasOS and fills in everything else: tcc as the compiler
and archiver, the rootfs paths, the target flags. `tcc -dumpdefaults` tells it which
FPU and FP runtime the new compiler should default to, so it matches the board the
running compiler was built for.

From a PC, `scripts/tinycc_board.py` in the YasOS repository keeps a copy on a board
and runs this there.

## Options that matter on the target

| Option | Effect |
|---|---|
| `--enable-O0` | Build at `-O0`: about 1 minute instead of 15 on the board, a compiler 35% bigger |
| `--disable-optimizer` | A compiler that only does `-O0`, half the size. `-O1` and up are accepted, have no effect, and print a note |
| `--enable-low-mem` | Smaller limits and tables for a part without external RAM (128-entry value stack, smaller hash tables), and no `-fmodule-local-calls` by default, which makes its output about 5% bigger |

`--disable-optimizer` does not change `-O0` code: a compiler built with it, given
`-fmodule-local-calls`, builds a tinycc byte-identical to the one a full compiler
builds at `-O0`.

An optimizer-less compiler cannot make the small `-O2` build of itself: built by itself
it is about 1.27 MB, built by the cross at `-O2` 0.91 MB (a full compiler: 2.49 and
1.85 MB).

## Makefile portability

The device runs GNU make 4.4.90, whose `$\` also removes the whitespace before it, so
a space that has to survive a line break cannot sit in front of `$\`. There is no `ar -M`
and no `git` on the target.
