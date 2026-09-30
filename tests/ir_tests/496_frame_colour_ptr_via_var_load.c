/* frame_colour gives frame objects whose lifetimes do not overlap the same
   bytes, and an object whose address goes somewhere it cannot follow -- into
   memory -- escapes and lives to the end.  The address of t0 (a by-value
   argument's copy) sits in t1, a VAR read in two blocks, so SSA leaves it a
   VAR, and each read is `T <-- V [LOAD]`: a move of the VAR's value.  The
   pointer flow followed ASSIGN, ADD and the like but not that LOAD, so the
   pointer stored into t13.tree carried no object and escaped nothing; t0's
   lifetime ended at its last direct use, and mk()'s struct results were
   placed over it while t13.tree still pointed there.  This is Zig's
   AstGen.generate storing `&tree` into its AstGen state; the tcc -O1 Zig
   compiler segfaulted reporting a syntax error. */
#include <stdio.h>

struct Alloc
{
  void *ptr;
  const void *vt;
};
struct Ast
{
  int a[16];
};
struct Gen
{
  struct Alloc gpa;
  const struct Ast *tree;
  int x;
};
struct Big
{
  int v[16];
};

__attribute__((noinline)) struct Big mk(int i)
{
  struct Big b;
  for (int k = 0; k < 16; k++)
    b.v[k] = i * 100 + k;
  return b;
}

__attribute__((noinline)) int annotate(struct Alloc a, struct Ast t) { return t.a[4] + (a.ptr != 0); }

__attribute__((noinline)) int use(struct Gen *g) { return g->x + g->tree->a[1]; }

__attribute__((noinline)) int generate(struct Alloc const a0, struct Ast const a1)
{
  struct Gen t13;
  struct Ast t0;
  const struct Ast *t1;
  const struct Ast **t15;
  struct Big t2, t3, t4;
  int s, t9;
  t0 = a1;
  t1 = (const struct Ast *)&t0;
  s = t1->a[15];
  t9 = annotate(a0, a1);
  t13.gpa = a0;
  if (t9 > 0)
  {
    t15 = (const struct Ast **)&t13.tree;
    (*t15) = t1;
  }
  else
  {
    t15 = (const struct Ast **)&t13.tree;
    (*t15) = t1 + 0;
    s += t1->a[3];
  }
  t13.x = s + t9;
  s = use(&t13);
  t2 = mk(1);
  t3 = mk(2);
  t4 = mk(3);
  s += t2.v[3] + t3.v[5] + t4.v[7];
  return s + t13.tree->a[13] + t13.tree->a[2];
}

int main(void)
{
  struct Ast a;
  struct Alloc al = {&a, 0};
  for (int k = 0; k < 16; k++)
    a.a[k] = 1000 * k;
  printf("%d\n", generate(al, a));
  return 0;
}
