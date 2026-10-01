/* A byte/halfword local read through its own stack slot keeps its width.
 *
 * lea_fold turns `T = &local; ... *T` into a direct stack-slot operand, and the
 * value paths loaded such a slot with a word load meant for register spill
 * slots.  Reading a 2-byte struct's u16 field then pulled in the neighbouring
 * local's bytes: Zig's C backend returns `!void` as exactly such a struct, and
 * `if (t7.error)` took the error branch on a success (-O1/-O2). */
#include <stdio.h>
#include <stdint.h>

struct error_union /* Zig's `!void` */
{
  uint16_t error;
};
struct slice
{
  const char *ptr;
  unsigned len;
};
struct flag
{
  uint8_t set;
};

static int side;

__attribute__((noinline)) static struct error_union may_fail(int fail)
{
  struct error_union r = {.error = fail ? 7 : 0};
  return r;
}

/* The error test must see only the 16-bit field. */
__attribute__((noinline)) static struct error_union try_step(struct slice const a1, int fail)
{
  struct slice t2;
  struct error_union t7;
  t2 = a1;
  t7 = may_fail(fail);
  if (t7.error)
    return t7;
  side += (int)t2.len; /* the success path must run */
  return (struct error_union){.error = 0};
}

/* The same field passed on as an argument must arrive zero-extended. */
__attribute__((noinline)) static unsigned widen(uint32_t v)
{
  return v;
}
__attribute__((noinline)) static unsigned error_code(struct slice const a1, int fail)
{
  struct slice t2;
  struct error_union t7;
  t2 = a1;
  t7 = may_fail(fail);
  return widen(t7.error) + t2.len * 0u;
}

__attribute__((noinline)) static struct flag get_flag(int v)
{
  struct flag f = {.set = (uint8_t)v};
  return f;
}
__attribute__((noinline)) static int flag_is_set(struct slice const a1, int v)
{
  struct slice t2;
  struct flag t5;
  t2 = a1;
  t5 = get_flag(v);
  if (t5.set)
    return 1 + (int)(t2.len * 0u);
  return 0;
}

int main(void)
{
  struct slice s = {"abc", 5};
  struct error_union a = try_step(s, 0);
  struct error_union b = try_step(s, 1);
  printf("%u %u %d\n", a.error, b.error, side); /* 0 7 5 */
  printf("%u %u\n", error_code(s, 0), error_code(s, 1)); /* 0 7 */
  printf("%d %d\n", flag_is_set(s, 0), flag_is_set(s, 1)); /* 0 1 */
  return 0;
}
