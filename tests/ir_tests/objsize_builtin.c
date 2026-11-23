/* __builtin_object_size / __builtin_dynamic_object_size: the frontend's
 * answers (builtin/objsize.c), the same at every -O level -- also through an
 * inline function, whose parameter is never looked at. */
#include <stdio.h>
#include <stdlib.h>

#define P(e) printf("%s = %d\n", #e, (int)(e))
#define B(p, t) __builtin_object_size(p, t)
#define D(p, t) __builtin_dynamic_object_size(p, t)

struct S
{
  int a;
  char buf[10];
  int b;
};
struct F
{
  int n;
  char d[];
};
char g[20];
struct S gs;
void *xalloc(size_t a, size_t b) __attribute__((alloc_size(1, 2)));
void *xalloc(size_t a, size_t b)
{
  return malloc(a * b);
}

static inline size_t inl(char *p)
{
  return __builtin_object_size(p, 0);
}

__attribute__((noinline)) void members(struct S *ps, struct F *pf, char *q)
{
  char a[10];
  struct S s;
  char m[3][10];
  P(B(a, 0));
  P(B(&a[3], 1));
  P(B(a + 12, 0));
  P(B(g + 5, 0));
  P(B(s.buf, 0));
  P(B(s.buf, 1));
  P(B(&s.buf[2], 1));
  P(B(&s.buf[2], 3));
  P(B(&s.b, 0));
  P(B(gs.buf, 1));
  P(B(ps->buf, 0));
  P(B(ps->buf, 1));
  P(B(ps->buf, 2));
  P(B(ps->buf + 4, 3));
  P(B(pf->d, 1));
  P(B(q, 0));
  P(B(q, 2));
  P(B("abc", 0));
  P(B(m[1], 1));
  P(B((char *)&s + 4, 1));
  P(inl(a));
  P(inl("hello"));
}

__attribute__((noinline)) void allocs(int c, size_t n)
{
  char *p = malloc(10);
  char *p2 = calloc(3, 4);
  char *p3 = xalloc(5, 6);
  char *p4 = malloc(100);
  char *p5 = malloc(n);
  char *p6 = calloc(n, 4);
  char vla[n + 1];
  if (c)
    p4 = realloc(p4, 20);
  P(B(p, 0));
  P(B(p + 3, 0));
  P(B(p, 2));
  P(B(p2, 0));
  P(B(p3, 1));
  P(B(p4, 0));
  P(B(p4, 2));
  P(B(p5, 0));
  P(B(p5, 2));
  P(D(p5, 0));
  P(D(p6, 2));
  P(D(p, 0));
  P(B(vla, 0));
  P(D(vla, 0));
  P(inl(p));
  for (int i = 0; i < 1; i++)
    P(B(p, 0)); /* p is declared outside the loop: unknown */
  free(p);
  free(p2);
  free(p3);
  free(p4);
  free(p5);
  free(p6);
}

int main(void)
{
  struct S s;
  members(&s, 0, g);
  allocs(1, 7);
  return 0;
}
