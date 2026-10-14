/* `x = f(...)` and `T x = f(...)` hand x to f as its struct-return buffer:
   only sound when the call's result IS the value stored.  A call among the
   arguments, behind a comma, inside a statement expression or a brace
   initializer must not write x early; a destination at an unaligned address
   (a packed member) is not handed to a callee that builds its result there
   with word copies. */
#include <stdio.h>
#include <string.h>

struct S
{
  unsigned a, b, c, d, e, f;
};
struct W
{
  struct S s;
};
struct __attribute__((packed)) P
{
  char tag;
  struct S s;
  char tail;
};

__attribute__((noinline)) static struct S mk(unsigned k)
{
  struct S r = {k, k + 1, k + 2, k + 3, k + 4, k + 5};
  return r;
}
/* built in the caller's buffer from a word-aligned image: copies that use
   LDM/STM there */
__attribute__((noinline)) static struct S mkc(unsigned k)
{
  static const struct S img = {11, 12, 13, 14, 15, 16};
  struct S r = img;
  r.c = k;
  return r;
}
__attribute__((noinline)) static struct S *self(struct S v, struct S *p)
{
  p->f += v.a;
  return p;
}
static const struct S K = {9, 9, 9, 9, 9, 9};

__attribute__((noinline)) static unsigned t_brace(int c)
{
  struct S x = {mk(1).b, 7};
  return x.a * 1000 + x.b * 100 + x.c * 10 + x.f + c;
}
__attribute__((noinline)) static unsigned t_wrap(int c)
{
  struct W w = {mk(2)};
  return w.s.a * 10 + w.s.f + c;
}
__attribute__((noinline)) static unsigned t_arg(int c)
{
  struct S x = {1, 2, 3, 4, 5, 6};
  x = *self(mk(10), &x);
  return x.a * 100 + x.f + c;
}
__attribute__((noinline)) static unsigned t_comma(int c)
{
  struct S x = {1, 2, 3, 4, 5, 6};
  x = ((void)mk(10), x);
  return x.a * 100 + x.f + c;
}
__attribute__((noinline)) static unsigned t_stmt_expr(int c)
{
  struct S x = {1, 2, 3, 4, 5, 6};
  x = ({
    mk(10);
    x;
  });
  return x.a * 100 + x.f + c;
}
__attribute__((noinline)) static unsigned t_decl_comma(int c)
{
  struct S x = {1, 2, 3, 4, 5, 6};
  struct S y = (mk(20), x);
  return y.a * 100 + y.f + c;
}
__attribute__((noinline)) static struct S pick(int c)
{
  struct S x;
  x = c ? mk(1) : K;
  return x;
}
__attribute__((noinline)) static struct S pick2(int c)
{
  struct S x = c ? mk(2) : mk(3);
  return x;
}
__attribute__((noinline)) static unsigned t_packed(int c)
{
  struct P p;
  memset(&p, 0, sizeof p);
  p.tag = 1;
  p.s = mk(30 + c);
  p.tail = 2;
  unsigned r = p.s.a * 1000 + p.s.f * 10 + p.tag + p.tail;
  p.s = mkc(50 + c);
  return r + p.s.c * 100000 + p.s.f + p.tag + p.tail;
}
struct S (*volatile fpv)(unsigned) = mk;
__attribute__((noinline)) static unsigned t_fnptr(int c)
{
  struct S x = {1, 2, 3, 4, 5, 6};
  x = fpv(40 + c);
  return x.a * 100 + x.f;
}

int main(void)
{
  printf("%u %u %u %u %u %u\n", t_brace(0), t_wrap(0), t_arg(0), t_comma(0), t_stmt_expr(0), t_decl_comma(0));
  for (int c = 0; c < 2; c++)
  {
    struct S a = pick(c), b = pick2(c);
    printf("%u %u %u %u %u %u\n", a.a, a.f, b.a, b.f, t_packed(c), t_fnptr(c));
  }
  return 0;
}
