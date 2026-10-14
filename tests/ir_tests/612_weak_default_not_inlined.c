// yaslibc's no-OS sbrk() forwards to a weak ENOSYS `_sbrk` defined beside it,
// and the embedding program (the YasOS kernel) supplies the real one.  tcc
// inlined the weak default into sbrk(), so the kernel built by tcc failed
// every allocation ("Filesystem initialization failed: OutOfMemory").  A weak
// definition is replaceable at link time: callers must call through the
// symbol -- no inlining, no constant result, no purity taken from its body.
// The strong definitions live in 612+_weak_default_not_inlined.c.
#include <stdio.h>

__attribute__((weak)) void *_sbrk_hook(int increment)
{
  (void)increment;
  return (void *)-1;
}

void *sbrk_front(int increment) { return _sbrk_hook(increment); }

__attribute__((weak)) int weak_value(void) { return 1; }

static int via_static(void) { return weak_value(); }

int use_value(void) { return weak_value() + 1; }

int main(void)
{
  void *p = sbrk_front(16);
  printf("sbrk=%s\n", p == (void *)-1 ? "default" : "override");
  printf("value=%d static=%d\n", use_value(), via_static());
  return 0;
}
