/* ra:exit_sink: a value computed every loop iteration but read only in a
 * loop-exit block (the early `return i` of Zig's `for (list[start..], start..)`)
 * sinks into that block instead.  Cases: the original fall-through-exit loop,
 * an early return past a second test (two sinks, two exit blocks), a nested
 * inner loop's exit, and a value also read in the body, which must stay put. */
#include <stdio.h>

typedef unsigned u32;
typedef unsigned char u8;

__attribute__((noinline)) u32 first_at_least(const u32 *list, u32 len, u32 start, u32 end, u32 *out)
{
  const u32 *p = list + start;
  for (u32 k = 0; k < len - start; k++) {
    u32 i = start + k;
    if (p[k] >= end)
      return i;
    out[k] = p[k];
  }
  return len;
}

__attribute__((noinline)) u32 two_exits(const u32 *a, u32 n, u32 lo, u32 hi)
{
  for (u32 k = 0; k < n; k++) {
    u32 i = 100 + k;
    u32 j = 200 + k;
    if (a[k] < lo)
      return i;
    if (a[k] > hi)
      return j;
  }
  return 0;
}

__attribute__((noinline)) u32 nested_pos(const u8 *m, u32 rows, u32 cols, u8 want)
{
  for (u32 r = 0; r < rows; r++)
    for (u32 c = 0; c < cols; c++) {
      u32 pos = 1000 * r + c;
      if (m[r * cols + c] == want)
        return pos;
    }
  return 12345;
}

/* i is also accumulated in the body: sinking it would be wrong, so this
 * checks the pass leaves it (and the result) alone. */
__attribute__((noinline)) u32 sum_and_first_ge(const u32 *a, u32 n, u32 end)
{
  u32 acc = 0;
  for (u32 k = 0; k < n; k++) {
    u32 i = 5 + k;
    acc += i * (a[k] & 7);
    if (a[k] >= end)
      return i * 1000000 + acc;
  }
  return acc;
}

/* the exit block is entered by the taken branch (a goto out), exercising the
 * sunk instruction's landing fixup on a jump edge */
__attribute__((noinline)) u32 goto_exit(const u32 *a, u32 n, u32 base, u32 want)
{
  u32 i = 0;
  for (u32 k = 0; k < n; k++) {
    i = base + k;
    if (a[k] == want)
      goto out;
  }
  return 0;
out:
  return i;
}

int main(void)
{
  u32 list[6] = {1, 3, 5, 7, 9, 11};
  u32 out[6];
  u32 grid[3][3] = {{10, 20, 30}, {40, 50, 60}, {70, 80, 90}};

  printf("%u %u %u %u\n", first_at_least(list, 6, 0, 6, out),
         first_at_least(list, 6, 2, 10, out), first_at_least(list, 6, 0, 99, out),
         first_at_least(list, 6, 6, 0, out));
  printf("%u %u %u %u\n", out[0], out[1], out[2], out[3]);

  printf("%u %u %u %u\n", two_exits(list, 6, 2, 8), two_exits(list, 6, 10, 8),
         two_exits(list, 6, 0, 10), two_exits(list, 6, 5, 5));

  printf("%u %u %u\n", nested_pos((const u8 *)grid, 3, 3, 50),
         nested_pos((const u8 *)grid, 3, 3, 255), nested_pos((const u8 *)grid, 3, 3, 10));

  printf("%u %u %u\n", sum_and_first_ge(list, 6, 7), sum_and_first_ge(list, 6, 99),
         sum_and_first_ge(list, 6, 5));

  printf("%u %u %u\n", goto_exit(list, 6, 100, 5), goto_exit(list, 6, 100, 99),
         goto_exit(list, 6, 100, 11));
  return 0;
}
