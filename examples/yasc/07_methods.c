/* Chapter 7 -- methods (README.md).
 *
 * A function written in a struct body is a method.  It takes no room in the
 * struct: it is the ordinary function TAG__NAME (here stack__push, ...), so C
 * code can call it by that name.  Its first parameter, written out, is the
 * object: `s.push(x)` passes &s, `p->push(x)` passes p.  A method without
 * one is called through the struct's name: `stack.create(4)`.
 * Build: tcc 07_methods.c mem.o -o methods */
#pragma yasc
#include <stdio.h>
#include "mem.h"

struct stack
{
  int * owned items;
  int len, cap;

  /* no self: stack.create(...) */
  struct stack * owned maybe create(int cap)
  {
    struct stack * owned maybe s = mem_new(sizeof *s);
    if (!s)
      return s;
    int * owned maybe items = mem_new(cap * sizeof(int));
    if (!items)
    {
      mem_free(s);
      return NULL;
    }
    s->items = move(items);
    s->len = 0;
    s->cap = cap;
    return s;
  }

  /* a view self: it looks, or changes the fields, and never frees */
  int push(struct stack * view self, int v)
  {
    if (self->full())   /* a body may call any method of the struct */
      return -1;
    self->items[self->len++] = v;
    return 0;
  }

  int pop(struct stack * view self) { return self->items[--self->len]; }

  int full(const struct stack * view self) { return self->len == self->cap; }

  int empty(const struct stack * view self) { return self->len == 0; }

  /* a drop self: the call consumes the object, as mem_free would */
  void destroy(drop struct stack * owned self)
  {
    mem_free(self->items);
    mem_free(self);
  }
};

/* A small struct passed by value: the method gets a copy. */
struct range
{
  int from, to;
  int length(struct range self) { return self.to - self.from; }
  int contains(const struct range * view self, int v) { return v >= self->from && v < self->to; }
};

int main(void)
{
  struct stack * owned maybe s = stack.create(3);
  if (!s)
    return 1;
  for (int i = 1; i <= 4; i++)
    if (s->push(i * 10) != 0)
      printf("full at %d\n", i * 10);
  while (!s->empty())
    printf("pop %d\n", s->pop());
  s->destroy();   /* s owns nothing now: using it again would be caught */

  struct range r = {5, 9};
  printf("range %d, has 7: %d, has 9: %d\n", r.length(), r.contains(7), r.contains(9));
  printf("by name: %d\n", range__length(r));
  printf("blocks left: %d\n", mem_live());
  return 0;
}
