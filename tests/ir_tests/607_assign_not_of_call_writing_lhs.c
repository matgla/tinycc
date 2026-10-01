// toybox awk's logical NOT is "STKP->num = ! get_set_logical();", where the
// callee also rewrites STKP->num (and its flags). The store of the negated
// result has to land last. On the device `!0` came out 0 and every awk loop
// body was skipped.
#include <stdio.h>

struct zvalue {
  unsigned flags;
  double num;
  void *p;
};

static struct zvalue stack[4];
static struct zvalue *stackp = stack;

__attribute__((noinline)) static int get_set_logical(void)
{
  struct zvalue *v = stackp;
  int r = !!v->num;

  v->num = r;
  v->flags = 1;
  return r;
}

static int get_set_logical_inl(void)
{
  struct zvalue *v = stackp;
  int r = !!v->num;

  v->num = r;
  v->flags = 1;
  return r;
}

int main(void)
{
  stackp->num = 0;
  stackp->num = ! get_set_logical();
  printf("not0=%d\n", (int)stackp->num);
  stackp->num = 5;
  stackp->num = ! get_set_logical();
  printf("not5=%d\n", (int)stackp->num);
  stackp->num = 0;
  stackp->num = ! get_set_logical_inl();
  printf("inl not0=%d\n", (int)stackp->num);
  stackp->num = 2;
  stackp->num = ! get_set_logical_inl();
  printf("inl not2=%d\n", (int)stackp->num);
  stackp->num = 0;
  (stackp)->num = ! get_set_logical_inl();
  int t = get_set_logical_inl();
  printf("while-shape t=%d\n", t);
  return 0;
}
