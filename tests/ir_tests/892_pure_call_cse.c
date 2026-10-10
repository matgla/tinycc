/* ssa:pure_call_cse + purity:transitive: a call to a callee that writes no
 * memory, repeating an earlier call in its block with the same arguments and
 * nothing in between that could change what it reads, reuses the earlier
 * result.  The positive case is the Zig symbol-table walk (`size(p)` then
 * `next(p) = p + size(p)`); the rest must keep both calls. */
#include <stdint.h>

typedef uintptr_t usize;

static int G;

__attribute__((noinline)) static usize find_sentinel(const unsigned char *p)
{
  usize i = 0;
  while (p[i] != 0)
    i++;
  return i;
}

/* PURE only through find_sentinel's cached verdict. */
__attribute__((noinline)) static usize sym_size(const unsigned char *s, unsigned char align)
{
  usize n = find_sentinel(s + 4) + 5;
  usize a = align ? align : 1;
  return (n + a - 1) & ~(a - 1);
}

static const unsigned char *sym_next(const unsigned char *s, unsigned char align)
{
  return s + sym_size(s, align);
}

__attribute__((noinline)) static usize walk(const unsigned char *p, usize n, const unsigned char *alignp)
{
  usize total = 0;
  for (usize i = 0; i < n; i++)
  {
    total += sym_size(p, *alignp);
    p = sym_next(p, *alignp);
  }
  return total;
}

__attribute__((noinline)) static int sq(int v)
{
  return v * v + 1;
}

__attribute__((noinline)) static int read_g(int k)
{
  return G + k;
}

__attribute__((noinline)) static void append(unsigned char *s)
{
  usize n = find_sentinel(s);
  s[n] = 'z';
  s[n + 1] = 0;
}

__attribute__((noinline)) static void opaque(int **pp)
{
  (void)pp;
}

/* A store into the scanned string, through another pointer, between the
 * calls. */
__attribute__((noinline)) static usize store_between(const unsigned char *s, unsigned char *alias)
{
  usize a = find_sentinel(s);
  alias[a] = 'y';
  alias[a + 1] = 0;
  return find_sentinel(s) * 100 + a;
}

/* An impure call between the calls. */
__attribute__((noinline)) static usize call_between(unsigned char *s)
{
  usize a = find_sentinel(s);
  append(s);
  return find_sentinel(s) * 100 + a;
}

/* The argument is an address-taken local changed through a pointer. */
__attribute__((noinline)) static int through_pointer(int x)
{
  int *p = &x;
  opaque(&p);
  int a = sq(x);
  *p = x + 1;
  int b = sq(x);
  return b - a;
}

/* The callee reads a global written between the calls. */
__attribute__((noinline)) static int global_between(void)
{
  G = 3;
  int a = read_g(1);
  G = 7;
  int b = read_g(1);
  return b * 10 + a;
}

/* Same callee, different arguments; and an argument redefined. */
__attribute__((noinline)) static usize other_args(const unsigned char *s)
{
  usize a = find_sentinel(s);
  usize b = find_sentinel(s + 1);
  s += 2;
  usize c = find_sentinel(s);
  return a * 100 + b * 10 + c;
}

/* Pure calls in between do not stop the reuse, and the result is right. */
__attribute__((noinline)) static usize pure_between(const unsigned char *s)
{
  usize a = find_sentinel(s);
  int q = sq((int)a);
  usize b = find_sentinel(s);
  return a + b + (usize)q;
}

int main(void)
{
  /* Records: 4-byte header, NUL-terminated name, padded to `align`:
   * round_up(4 + len + 1, 4) = 8, 8, 12. */
  static const char *const names[] = {"abc", "x", "longer"};
  unsigned char table[64] = {0};
  usize off = 0, expect = 0;
  for (int r = 0; r < 3; r++)
  {
    usize len = 0;
    while (names[r][len])
      len++;
    for (usize k = 0; k < len; k++)
      table[off + 4 + k] = (unsigned char)names[r][k];
    usize size = (len + 5 + 3) & ~(usize)3;
    off += size;
    expect += size;
  }
  unsigned char align = 4;
  if (expect != 28 || walk(table, 3, &align) != expect)
    return 1;

  unsigned char s1[8] = "ab";
  if (store_between(s1, s1) != 3 * 100 + 2)
    return 2;
  unsigned char s2[8] = "abc";
  if (call_between(s2) != 4 * 100 + 3)
    return 3;
  if (through_pointer(4) != (5 * 5 + 1) - (4 * 4 + 1))
    return 4;
  if (global_between() != 8 * 10 + 4)
    return 5;
  if (other_args((const unsigned char *)"hello") != 5 * 100 + 4 * 10 + 3)
    return 6;
  if (pure_between((const unsigned char *)"four") != 4 + 4 + 17)
    return 7;
  return 0;
}
