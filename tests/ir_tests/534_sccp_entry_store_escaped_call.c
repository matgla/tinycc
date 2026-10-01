#include <stdio.h>

/* A local initialised in the entry block, whose address then goes to a call
 * that writes it, must be re-read after the call.  SCCP kept the initializer
 * for an entry-block store across the call (the entry-block alias scan did
 * not treat calls as writers), so `d.backend == 0` folded to true and the
 * returned fields were the zeros -- ctags' lregex choose_backend returned a
 * NULL backend and faulted on `desc.backend->fdefs`. */
struct desc
{
  const char *backend;
  unsigned flags;
  int type;
};

__attribute__((noinline)) void eval(const char *f, struct desc *d)
{
  if (f[0] == 'x')
    return;
  d->backend = f;
  d->flags |= 0x10;
}

__attribute__((noinline)) struct desc choose(const char *flags, int type)
{
  struct desc d = { .backend = 0, .flags = 0, .type = type };
  if (flags)
    eval(flags, &d);
  if (d.backend == 0)
    eval("default", &d);
  return d;
}

int main(void)
{
  struct desc a = choose("pcre2", 1);
  struct desc b = choose("x", 2);
  struct desc c = choose(0, 3);
  printf("%s %u %d\n", a.backend, a.flags, a.type);
  printf("%s %u %d\n", b.backend, b.flags, b.type);
  printf("%s %u %d\n", c.backend, c.flags, c.type);
  return 0;
}
