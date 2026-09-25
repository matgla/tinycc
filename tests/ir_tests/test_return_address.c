/* __builtin_return_address(0) reads the LR the prologue saved, off SP, without
 * a frame pointer.  One callee per prologue shape: a leaf (LR saved only for
 * the builtin), a non-leaf, stack arguments, a struct straddling r3 and the
 * stack (r0-r3 pushed first), variadic (r0-r3 save + frame pointer), alloca
 * (frame-record layout), a frame past 4 KB, a read after a call, and a tail
 * call (its teardown must pop the LR the builtin made it save).  Each
 * returns its return address; its caller checks that it lands a little past
 * the caller's own PC, read just before the call, and two call sites must
 * differ.  The PC is read with inline asm, not taken from `&caller`: on YasOS a
 * function pointer is the loader's r9-setting thunk, not the code address. */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define NOINLINE __attribute__((noinline))

struct five
{
  int v[5];
};

static volatile int sunk;

NOINLINE static void sink(int x)
{
  sunk += x;
}

NOINLINE static void *leaf(void)
{
  return __builtin_return_address(0);
}

NOINLINE static void *nonleaf(int x)
{
  sink(x);
  return __builtin_return_address(0);
}

NOINLINE static void *stack_args(int a, int b, int c, int d, int e, int f)
{
  sink(a + b + c + d + e + f);
  return __builtin_return_address(0);
}

NOINLINE static void *split_struct(int a, int b, int c, struct five s)
{
  sink(a + b + c + s.v[0] + s.v[4]);
  return __builtin_return_address(0);
}

NOINLINE static void *variadic(int n, ...)
{
  va_list ap;
  int total = 0;
  va_start(ap, n);
  while (n-- > 0)
    total += va_arg(ap, int);
  va_end(ap);
  sink(total);
  return __builtin_return_address(0);
}

NOINLINE static void *with_alloca(int n)
{
  char *p = __builtin_alloca(n);
  memset(p, 1, n);
  sink(p[n - 1]);
  return __builtin_return_address(0);
}

NOINLINE static void *big_frame(int i)
{
  volatile char buf[6000];
  buf[i] = 1;
  sink(buf[i]);
  return __builtin_return_address(0);
}

NOINLINE static void *after_call(void)
{
  sink(1);
  void *r = __builtin_return_address(0);
  sink(2);
  return r;
}

NOINLINE static void *pass(void *p)
{
  return p;
}

NOINLINE static void *tail_call(void)
{
  return pass(__builtin_return_address(0));
}

/* A code address in the caller: the PC, which reads 4 past the mov. */
#define HERE() ({ void *here_; __asm__ volatile("mov %0, pc" : "=r"(here_)); here_; })

/* A return address lands inside its caller, a little past `here`. */
static int near(void *ra, void *here)
{
  unsigned long d = (unsigned long)ra - (unsigned long)here;
  return d > 0 && d < 256;
}

NOINLINE static int call_leaf(void)
{
  void *here = HERE();
  return near(leaf(), here);
}
NOINLINE static int call_nonleaf(void)
{
  void *here = HERE();
  return near(nonleaf(3), here);
}
NOINLINE static int call_stack_args(void)
{
  void *here = HERE();
  return near(stack_args(1, 2, 3, 4, 5, 6), here);
}
NOINLINE static int call_split_struct(void)
{
  struct five s = {{1, 2, 3, 4, 5}};
  void *here = HERE();
  return near(split_struct(7, 8, 9, s), here);
}
NOINLINE static int call_variadic(void)
{
  void *here = HERE();
  return near(variadic(3, 1, 2, 3), here);
}
NOINLINE static int call_with_alloca(void)
{
  void *here = HERE();
  return near(with_alloca(40), here);
}
NOINLINE static int call_big_frame(void)
{
  void *here = HERE();
  return near(big_frame(5000), here);
}
NOINLINE static int call_after_call(void)
{
  void *here = HERE();
  return near(after_call(), here);
}
NOINLINE static int call_tail_call(void)
{
  void *here = HERE();
  return near(tail_call(), here);
}

NOINLINE static int two_sites(void)
{
  void *a = leaf();
  void *b = leaf();
  return a != b;
}

int main(void)
{
  printf("%d %d %d %d\n", call_leaf(), call_nonleaf(), call_stack_args(), call_split_struct());
  printf("%d %d %d %d %d %d\n", call_variadic(), call_with_alloca(), call_big_frame(), call_after_call(),
         call_tail_call(), two_sites());
  return 0;
}
