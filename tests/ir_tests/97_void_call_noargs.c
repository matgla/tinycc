extern void printf(const char *format, ...);

static int bar(int x)
{
  return x + 1;
}

static void foo(void)
{
  /* Intentionally empty: 0-arg, void-return call site. */
}

int main(void)
{
  int a = bar(1);
  foo();
  printf("a=%d\n", a);
  return 0;
}
