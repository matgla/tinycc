/* A function whose one call is a tail call but which also returns earlier:
   the tail call becomes a branch and no epilogue was emitted, while the
   early returns still jumped to "the epilogue" -- the first instruction of
   whatever function the linker put next.  Zig's InternPool.Alignment.max
   (`if (a == .none) return b; if (b == .none) return a; return maxStrict(a,
   b);`) ran into the next function at -O1/-O2, and the tcc-built Zig
   compiler crashed at startup.  after() follows each function, so a missing
   epilogue shows up as its result. */
#include <stdint.h>
#include <stdio.h>

typedef uint8_t al;

__attribute__((noinline)) al max_strict(al a, al b) { return (a & 63) > (b & 63) ? a : b; }

__attribute__((noinline)) al amax(al const a0, al const a1)
{
  if (a0 == 63)
    return a1;
  if (a1 == 63)
    return a0;
  return max_strict(a0, a1);
}

__attribute__((noinline)) int after1(int x) { return x * 1000 + 7; }

static int sink;
__attribute__((noinline)) void note(int v) { sink += v; }

/* void flavour: an early `return;` before the tail call. */
__attribute__((noinline)) void maybe_note(int v)
{
  if (v < 0)
    return;
  note(v);
}

__attribute__((noinline)) int after2(int x) { return x * 2000 + 9; }

int main(void)
{
  printf("%u %u %u %u\n", amax(63, 5), amax(4, 63), amax(3, 9), amax(12, 2));
  maybe_note(-1);
  maybe_note(5);
  maybe_note(-7);
  printf("%d %d %d\n", sink, after1(1), after2(1));
  return 0;
}
