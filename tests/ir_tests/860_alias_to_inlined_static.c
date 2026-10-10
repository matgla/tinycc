/* An alias to a static function whose every call was inlined: the function
 * (called once, or called twice, or declared after the alias) still needs a
 * body of its own for the alias. */
int printf(const char *, ...);

static int once(int x) { return x * 7 + 3; }
int alias_once(int x) __attribute__((alias("once")));

static int twice(int x) { return x * 5 + 1; }
int alias_twice(int x) __attribute__((alias("twice")));

static int late(int x);
int alias_late(int x) __attribute__((alias("late")));
static int late(int x) { return x - 9; }

__attribute__((noinline)) int use(int a) { return once(a) + twice(a) + twice(a + 1) + late(a); }

int main(void)
{
  printf("%d\n", use(2));
  printf("%d %d %d\n", alias_once(2), alias_twice(2), alias_late(2));
  return 0;
}
