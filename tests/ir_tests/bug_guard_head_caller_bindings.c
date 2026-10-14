/* A guard head (docs/inline_guard_heads.md) replays the callee's test in the
 * caller.  Its free names -- the globals and typedefs it reads, and the
 * callee it calls -- must still mean the file-scope ones there.  Inside
 * another expansion the outer body's locals are in scope: `if (g)` read the
 * outer local `int g = 0` and skipped the call (wrong output at -O2), and a
 * head declaring `T v` with a typedef T stopped compiling under an outer
 * local `int T`. */
#include <stdio.h>

int g = 1;
unsigned hits;
typedef int T;

__attribute__((noinline)) void rec(int v) { hits = hits * 3 + v; }
#define WORK(v)                                                                                                        \
  rec(v);                                                                                                              \
  rec(1);                                                                                                              \
  rec(2);                                                                                                              \
  rec(3);                                                                                                              \
  rec(4);                                                                                                              \
  rec(5);                                                                                                              \
  rec(6);                                                                                                              \
  rec(7);                                                                                                              \
  rec(8)

/* Reads the global g in its test. */
static void drop(int *p)
{
  if (g)
  {
    WORK(*p);
    *p = 0;
  }
}

/* Declares a local through the file-scope typedef T. */
static void dt(int *p)
{
  T v = *p;
  if (v)
  {
    WORK(v);
    *p = 0;
  }
}

static inline __attribute__((always_inline)) void outer_ai(int *p)
{
  int g = 0;
  drop(p);
  hits += g;
}

static void outer_auto(int *p)
{
  int g = 0;
  drop(p);
  hits += g;
}

static void outer_typedef(int *p)
{
  int T = 2;
  dt(p);
  hits += T;
}

/* Two levels: the shadowing local sits in the outermost expansion. */
static void mid(int *p) { drop(p); }
static void outer_two(int *p)
{
  int g = 0;
  mid(p);
  hits += g;
}

/* The callee's own names may repeat the caller's locals freely. */
static void same_names(int *p)
{
  int v = 9;
  dt(p);
  hits += v;
}

int main(void)
{
  int x = 5, y = 7, z = 9, a = 3, w = 6, s = 2;
  outer_ai(&x);
  printf("A %u %d\n", hits, x);
  outer_auto(&y);
  printf("B %u %d\n", hits, y);
  drop(&z);
  printf("C %u %d\n", hits, z);
  outer_typedef(&a);
  dt(&a);
  printf("D %u %d\n", hits, a);
  outer_two(&w);
  printf("E %u %d\n", hits, w);
  same_names(&s);
  printf("F %u %d\n", hits, s);
  return 0;
}
