/* An indirect call whose target is also one of its own arguments.
 *
 * `return v(n, (void *)v);` in tail position: the allocator treated the
 * target as consumed by the PARAM that passes it and steered it into that
 * argument register; the call lowering then had to move the target out of
 * r0-r3 before placing the arguments, and in a tail-call-only function (LR not
 * saved) it found no free holding register and aborted with
 *   "func_call_mop: cannot find safe register to pre-save indirect call target"
 * on valid code at -O1/-O2/-Os.
 */
#include <stdio.h>

struct node;
typedef int (*vis_t)(struct node *, void *);
struct node { vis_t visit; int tag; };

static int calls;

__attribute__((noinline)) int visitor(struct node *n, void *self)
{
  calls++;
  /* the callee checks both arguments arrive intact */
  if (self != (void *)n->visit)
    return -1;
  return n->tag + 100;
}

/* tail position */
__attribute__((noinline)) int dispatch_tail(struct node *n)
{
  vis_t v = n->visit;
  return v(n, (void *)v);
}

/* tail position, target is the only argument (lands in r0) */
typedef int (*self_t)(void *);
struct selfnode { self_t visit; };

__attribute__((noinline)) int selfvisit(void *self)
{
  calls++;
  return self == (void *)selfvisit ? 7 : 8;
}

__attribute__((noinline)) int dispatch_self(struct selfnode *n)
{
  self_t v = n->visit;
  return v((void *)v);
}

/* same shape, not in tail position */
__attribute__((noinline)) int dispatch_nontail(struct node *n)
{
  vis_t v = n->visit;
  return v(n, (void *)v) + 1;
}

/* target passed twice, plus extra arguments */
typedef int (*vis4_t)(void *, void *, int, void *);
struct node4 { vis4_t visit; };

__attribute__((noinline)) int visitor4(void *a, void *b, int c, void *d)
{
  calls++;
  if (a != b || b != d)
    return -1;
  return c + 5;
}

__attribute__((noinline)) int dispatch4(struct node4 *n, int c)
{
  vis4_t v = n->visit;
  return v((void *)v, (void *)v, c, (void *)v);
}

int main(void)
{
  struct node n = { visitor, 11 };
  struct node4 n4 = { visitor4 };
  struct selfnode sn = { selfvisit };

  printf("tail=%d\n", dispatch_tail(&n));
  printf("nontail=%d\n", dispatch_nontail(&n));
  printf("self=%d\n", dispatch_self(&sn));
  printf("four=%d\n", dispatch4(&n4, 30));
  printf("calls=%d\n", calls);
  return 0;
}
