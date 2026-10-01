/* Tail merging by final location (ir/cross_jump.c tcc_ir_cross_jump_alloc).

   Block tails that differ only in which vreg brings a value in are merged
   once allocation is final, when both vregs sit in the same place; tails
   ending in a return merge too, and so do tails holding whole calls.  Each
   shape here is repeated so that a merge happens, and each path must still
   deliver its own value: the error code of the call that failed, the right
   argument to a shared call, the return value of its own path.

   Must not merge (or must merge correctly): tails whose incoming values sit
   in different places, and a shared call whose first arguments are set up
   before the common part. */
#include <stdio.h>

typedef unsigned short u16;

struct eu
{
  u16 error;
  unsigned payload;
};

static int log_count;

__attribute__((noinline)) u16 step(int x, int fail_at)
{
  return (u16)(x == fail_at ? 40 + x : 0);
}

__attribute__((noinline)) void note(unsigned v)
{
  log_count += (int)v;
}

/* Zig's `try` chain: every failure stores its own error through the sret
   pointer and leaves by the same exit. */
__attribute__((noinline)) struct eu chain(int fail_at)
{
  struct eu r;
  u16 e = step(1, fail_at);
  if (e)
  {
    r.error = e;
    r.payload = 0xaaaaaaaau;
    return r;
  }
  e = step(2, fail_at);
  if (e)
  {
    r.error = e;
    r.payload = 0xaaaaaaaau;
    return r;
  }
  e = step(3, fail_at);
  if (e)
  {
    r.error = e;
    r.payload = 0xaaaaaaaau;
    return r;
  }
  r.error = 0;
  r.payload = 7;
  return r;
}

/* Tails with a whole call: note(e) then return e. */
__attribute__((noinline)) unsigned calls(int fail_at)
{
  unsigned e = step(1, fail_at);
  if (e)
  {
    note(e);
    return e + 1000;
  }
  e = step(2, fail_at);
  if (e)
  {
    note(e);
    return e + 1000;
  }
  e = step(3, fail_at);
  if (e)
  {
    note(e);
    return e + 1000;
  }
  return 0;
}

/* The shared part starts after the first argument is set: the calls differ
   in it, so they must not collapse into one call with one argument. */
__attribute__((noinline)) unsigned two_arg(unsigned a, unsigned b)
{
  return a * 100u + b;
}

__attribute__((noinline)) unsigned split_args(int k, unsigned b)
{
  if (k == 1)
    return two_arg(11, b) + 5;
  if (k == 2)
    return two_arg(22, b) + 5;
  return two_arg(33, b) + 5;
}

/* Different incoming values in different places. */
__attribute__((noinline)) unsigned mixed(unsigned x, unsigned y, int k)
{
  unsigned r;
  if (k == 1)
  {
    r = x * 3u;
    note(r);
    return r + 1;
  }
  if (k == 2)
  {
    r = y * 5u;
    note(r);
    return r + 1;
  }
  r = x + y;
  note(r);
  return r + 1;
}

int main(void)
{
  for (int f = 0; f <= 4; f++)
  {
    struct eu r = chain(f);
    printf("chain(%d) error=%u payload=%x\n", f, r.error, r.payload);
  }
  for (int f = 0; f <= 4; f++)
  {
    unsigned v = calls(f);
    printf("calls(%d)=%u log=%d\n", f, v, log_count);
  }
  for (int k = 1; k <= 3; k++)
    printf("split_args(%d)=%u\n", k, split_args(k, 9));
  for (int k = 1; k <= 3; k++)
  {
    unsigned v = mixed(7, 11, k);
    printf("mixed(%d)=%u log=%d\n", k, v, log_count);
  }
  return 0;
}
