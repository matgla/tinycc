/* __builtin_return_address(0) reads the LR the prologue saved, off SP, without
 * a frame pointer.  One callee per prologue shape: a leaf (LR saved only for
 * the builtin), a non-leaf, stack arguments, a struct straddling r3 and the
 * stack (r0-r3 pushed first), variadic (r0-r3 save + frame pointer), alloca
 * (frame-record layout), a frame past 4 KB, a read after a call, and a tail
 * call (its teardown must pop the LR the builtin made it save).  Each
 * returns its return address; its caller checks that it lands just past the
 * caller's own entry, and two call sites must differ. */
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

/* A return address lands inside its caller, a little past the entry. */
static int near(void *ra, void (*fn)(void))
{
  unsigned long d = (unsigned long)ra - (unsigned long)fn;
  return d > 0 && d < 256;
}

NOINLINE static int call_leaf(void) { return near(leaf(), (void (*)(void))call_leaf); }
NOINLINE static int call_nonleaf(void) { return near(nonleaf(3), (void (*)(void))call_nonleaf); }
NOINLINE static int call_stack_args(void)
{
  return near(stack_args(1, 2, 3, 4, 5, 6), (void (*)(void))call_stack_args);
}
NOINLINE static int call_split_struct(void)
{
  struct five s = {{1, 2, 3, 4, 5}};
  return near(split_struct(7, 8, 9, s), (void (*)(void))call_split_struct);
}
NOINLINE static int call_variadic(void) { return near(variadic(3, 1, 2, 3), (void (*)(void))call_variadic); }
NOINLINE static int call_with_alloca(void) { return near(with_alloca(40), (void (*)(void))call_with_alloca); }
NOINLINE static int call_big_frame(void) { return near(big_frame(5000), (void (*)(void))call_big_frame); }
NOINLINE static int call_after_call(void) { return near(after_call(), (void (*)(void))call_after_call); }
NOINLINE static int call_tail_call(void) { return near(tail_call(), (void (*)(void))call_tail_call); }

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
