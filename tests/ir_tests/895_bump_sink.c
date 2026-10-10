/* ra:bump_sink: `T <- P + #k` moves below P's last read in its block, so P
 * and T share a register (and ra:store_postinc folds the pair into a
 * post-indexed access).  Every case below checks a shape the move must keep
 * intact: the base redefined between (a STORE-form def of a parameter too),
 * two bumps of one base, a bump whose result is read in the window, and the
 * ordinary pointer walks it exists for. */
#include <stdio.h>

typedef unsigned char u8;

__attribute__((noinline)) void fill(u8 *p, u8 b, unsigned n)
{
  for (; n; --n)
    *p++ = b;
}

__attribute__((noinline)) void copy(u8 *d, const u8 *s, unsigned n)
{
  for (; n; --n)
    *d++ = *s++;
}

__attribute__((noinline)) void fill_words(unsigned *p, unsigned w, unsigned n)
{
  for (; n >= 4; n -= 4)
    *p++ = w;
}

__attribute__((noinline)) int sum(const int *a, int n)
{
  int t = 0;
  while (n--)
    t += *a++;
  return t;
}

/* The base is reassigned (a STORE-form def of the parameter) after the bump. */
__attribute__((noinline)) int param_redef(int x)
{
  int a = x + 1;
  x = x * 3;
  int b = x + 1;
  return a * 100 + b;
}

/* Two bumps of one base must not trade places for ever. */
__attribute__((noinline)) int two_bumps(int *p)
{
  int *a = p + 1;
  int *b = p + 2;
  return *p + *a * 10 + *b * 100;
}

/* The bumped value itself is read before the base's last read. */
__attribute__((noinline)) int bump_read_early(int *p)
{
  int *q = p + 1;
  int v = *q;
  return v * 10 + *p;
}

/* Pointer stored and the old base read after: the stored value is the bump. */
__attribute__((noinline)) int *bump_store(int **slot, int *p)
{
  int *q = p + 1;
  *slot = q;
  *p = 7;
  return q;
}

/* A walk whose loop exit reads the incremented pointer. */
__attribute__((noinline)) unsigned span(const char *s)
{
  const char *p = s;
  while (*p++)
    ;
  return (unsigned)(p - s);
}

int main(void)
{
  u8 buf[20], src[20];
  unsigned words[6] = {0};
  int arr[5] = {1, 2, 3, 4, 5};
  int cell = 0, *slotv = 0;

  for (int i = 0; i < 20; i++) { buf[i] = 0; src[i] = (u8)(i * 7 + 1); }
  fill(buf + 3, 0xab, 9);
  copy(buf + 12, src + 2, 6);
  unsigned acc = 0;
  for (int i = 0; i < 20; i++) acc = acc * 31 + buf[i];
  printf("bytes=%u first=%d last=%d\n", acc, buf[3], buf[17]);

  fill_words(words + 1, 0x1234u, 17);
  printf("words=%x %x %x %x %x %x\n", words[0], words[1], words[2], words[3], words[4], words[5]);

  printf("sum=%d\n", sum(arr, 5));
  printf("param_redef=%d\n", param_redef(5));
  printf("two_bumps=%d\n", two_bumps(arr));
  printf("bump_read_early=%d\n", bump_read_early(arr + 2));
  int *r = bump_store(&slotv, &cell);
  printf("bump_store=%d %d %d\n", r == &cell + 1, slotv == &cell + 1, cell);
  printf("span=%u\n", span("hello"));
  return 0;
}
