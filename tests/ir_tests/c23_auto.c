/* C23 `auto` and GNU `__auto_type`: the type from the initializer, which is
 * evaluated once.  The QEMU build is -ccompat (C11), where only a yasc file
 * gives `auto` its C23 meaning; __auto_type infers everywhere. */
#pragma yasc
extern int printf(const char * view, ...);

struct pt { int x, y; };

static int calls;
static int next(void) { return ++calls * 10; }
static struct pt mk(int x, int y) { return (struct pt){x, y}; }
static int twice(int v) { return 2 * v; }
/* small enough for the inliner's constant evaluation, which must not read
   x as an int */
static inline int five(void) { auto x = 2.5; return (int)(x * 2); }

static int arr[3] = {4, 5, 6};
auto g = 7;
static __auto_type h = 2.5;

static long counter(void)
{
  static auto n = 100L;
  return n++;
}

int main(void)
{
  auto a = next();
  __auto_type b = next();
  printf("a=%d b=%d calls=%d\n", a, b, calls);

  auto d = a / 4.0;
  auto big = 1ULL << 40;
  printf("d=%d.%d big>>32=%u size=%u\n", (int)d, (int)(d * 10) % 10, (unsigned)(big >> 32), (unsigned)sizeof big);

  auto p = arr;
  p++;
  auto fp = twice;
  auto s = mk(3, 4);
  s.y += *p;
  printf("p=%d fp=%d s=%d,%d\n", *p, fp(*p), s.x, s.y);

  const auto k = (signed char)200;
  printf("k=%d size=%u\n", k, (unsigned)sizeof k);

  int sum = 0;
  for (auto i = 0u; i < 4; i++)
    sum += i;
  printf("sum=%d g=%d h*2=%d\n", sum, g, (int)(h * 2));

  long c1 = counter(), c2 = counter();
  printf("counter=%ld,%ld\n", c1, c2);

  auto str = "auto";
  printf("str=%s five=%d\n", str, five());
  return 0;
}
