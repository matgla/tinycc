/* A function whose only calls are noreturn is a leaf: LR is pushed in the cold
 * block right before the `bl`, keeping SP 8-aligned, not in the prologue.
 * A noreturn call with stack arguments keeps the prologue save.
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>

struct S
{
  unsigned a, depth, flags;
};
struct S s_lock;
static jmp_buf env;
static unsigned misaligned;
static long acc;

static void check_sp(void)
{
  uintptr_t sp;
  __asm__ volatile("mov %0, sp" : "=r"(sp));
  if (sp & 7)
    misaligned++;
}

__attribute__((noinline, noreturn)) void die(int code)
{
  check_sp();
  acc += code;
  longjmp(env, 1);
}

__attribute__((noinline, noreturn)) void die4(int a, int b, int c, int d)
{
  check_sp();
  acc += a * 1000 + b * 100 + c * 10 + d;
  longjmp(env, 1);
}

__attribute__((noinline, noreturn)) void die6(int a, int b, int c, int d, int e, int f)
{
  check_sp();
  acc += a + b + c + d + e * 100 + f * 1000;
  longjmp(env, 1);
}

__attribute__((noinline, noreturn)) void dief(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int x = va_arg(ap, int);
  int y = va_arg(ap, int);
  va_end(ap);
  check_sp();
  acc += (fmt[0] - 'a') * 10000 + x * 10 + y;
  longjmp(env, 1);
}

__attribute__((noinline)) void plain(int v) { acc += v * 7; }

/* The kheap_unlock shape: the hot path must not touch LR. */
__attribute__((noinline)) unsigned guard_unlock(void)
{
  struct S *p = &s_lock;
  if (p->depth == 0)
  {
    die4(4, 2, 1, 9);
    __builtin_unreachable();
  }
  p->depth = p->depth - 1;
  return p->depth;
}

/* The noreturn call laid out last. */
__attribute__((noinline)) unsigned guard_last(void)
{
  struct S *p = &s_lock;
  if (p->depth != 0)
  {
    p->depth = p->depth - 1;
    return p->depth;
  }
  die(5);
}

/* A frame with an address-taken local read into an argument right before
 * the call: SP-relative accesses around the push. */
__attribute__((noinline)) int guard_frame(int i, int v)
{
  volatile int buf[6];
  for (int k = 0; k < 6; k++)
    buf[k] = v + k;
  if (i < 0 || i >= 6)
    die4(buf[1], buf[2], buf[3], i & 7);
  return buf[i] * 2;
}

/* A frame and no callee-saved register: still a leaf, `sub sp` and `add sp`. */
__attribute__((noinline)) int guard_slots(int i)
{
  volatile int buf[2];
  buf[0] = i;
  buf[1] = i + 1;
  if (i > 100)
    die4(buf[0] & 7, buf[1] & 7, 3, 4);
  return buf[1];
}

/* Register pressure: callee-saved registers in the prologue, values live
 * into the noreturn call's arguments. */
__attribute__((noinline)) int guard_pressure(const int *a, int n)
{
  int s0 = 0, s1 = 1, s2 = 2, s3 = 3, s4 = 4, s5 = 5, s6 = 6;
  for (int k = 0; k < n; k++)
  {
    s0 += a[k];
    s1 ^= a[k] << 1;
    s2 += s0 >> 1;
    s3 -= a[k];
    s4 += s1 & 15;
    s5 ^= s2;
    s6 += s3 * 3;
    if (a[k] < 0)
      die4(s0 & 7, s1 & 7, s2 & 7, (s3 + s4 + s5 + s6) & 7);
  }
  return s0 + s1 + s2 + s3 + s4 + s5 + s6;
}

/* Two different noreturn guards. */
__attribute__((noinline)) int two_guards(int x, int y)
{
  if (x == 0)
    die(11);
  if (y > 100)
    dief("c", x, y & 7);
  return x * y;
}

/* Stack arguments to the noreturn callee: LR stays in the prologue. */
__attribute__((noinline)) int guard_stack_args(int x)
{
  if (x < 0)
    die6(1, 2, 3, 4, x & 7, 6);
  return x + 1;
}

/* An ordinary call next to the noreturn one. */
__attribute__((noinline)) int guard_and_call(int x)
{
  if (x < 0)
    die(13);
  plain(x);
  return x - 1;
}

#define TRY(expr)                                                                                                       \
  do                                                                                                                   \
  {                                                                                                                    \
    acc = 0;                                                                                                           \
    if (!setjmp(env))                                                                                                  \
    {                                                                                                                  \
      long r = (long)(expr);                                                                                           \
      printf("%-24s ret %ld\n", #expr, r);                                                                             \
    }                                                                                                                  \
    else                                                                                                               \
      printf("%-24s die %ld\n", #expr, acc);                                                                           \
  } while (0)

int main(void)
{
  static const int ok[5] = {1, 2, 3, 4, 5};
  static const int bad[5] = {1, 2, -3, 4, 5};
  s_lock.depth = 2;
  TRY(guard_unlock());
  TRY(guard_unlock());
  TRY(guard_unlock());
  s_lock.depth = 1;
  TRY(guard_last());
  TRY(guard_last());
  TRY(guard_frame(3, 10));
  TRY(guard_frame(7, 10));
  TRY(guard_frame(-1, 20));
  TRY(guard_slots(7));
  TRY(guard_slots(205));
  TRY(guard_pressure(ok, 5));
  TRY(guard_pressure(bad, 5));
  TRY(two_guards(3, 4));
  TRY(two_guards(0, 4));
  TRY(two_guards(5, 205));
  TRY(guard_stack_args(9));
  TRY(guard_stack_args(-3));
  TRY(guard_and_call(4));
  TRY(guard_and_call(-4));
  printf("misaligned %u\n", misaligned);
  return 0;
}
