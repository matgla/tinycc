/* Strong definitions replacing the weak defaults of 612_weak_default_not_inlined.c. */
static char heap[64];
static int brk;

void *_sbrk_hook(int increment)
{
  void *p = heap + brk;
  brk += increment;
  return p;
}

int weak_value(void) { return 41; }
