# `takes(p)`: a pointer moved out of a member through a pointer stays moved on the null path

**Status:** open · **Severity:** false positive (yasc lifetime) · **Found:** 2026-11-26 (libc grp.c's line buffer) · **Reported by:** claude-opus-5-5

## Summary

A `takes(v)` result that is null leaves `v` with the caller
(test_yasc_takes.py). That works for a local or a global, but not when
the local was itself moved out of a member reached through a pointer
parameter (`move(gf->line)`). On the null path, the caller's
`gf->line = move(old)` (or `free(old)`) is then reported as
`'old' is used after it was moved`, at every `-O`.

## Reproducer

```c
#pragma yasc
void free(drop void *?owned p);
void *?@(owned(free), takes(v)) realloc(void *?owned v, unsigned sz);
struct s { char *?owned(free) line; unsigned cap; };

static char *?owned(free) g;
_Bool global_ok(unsigned cap) {           /* clean */
  char *?owned(free) old = move(g);
  g = nullptr;
  char *?owned(free) bigger = realloc(move(old), cap);
  if (!bigger) { g = move(old); return 0; }
  g = move(bigger);
  return 1;
}

_Bool member_bad(inout struct s *gf, unsigned cap) {
  char *?owned(free) old = move(gf->line);
  gf->line = nullptr;
  char *?owned(free) bigger = realloc(move(old), cap);
  if (!bigger) {
    free(old);                            /* error: 'old' is used after it was moved */
    return 0;
  }
  free(bigger);
  return 1;
}
```

`armv8m-tcc -c t.c`: `member_bad` is reported, `global_ok` is not. The
only difference is where `old` came from.

## Workaround

libc grp.c (`gf_room`) grows its line buffer with malloc + memcpy + free
in place of realloc. Its comment names this report.
