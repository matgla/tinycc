/* yasc `defer`: the statement runs when its scope is left, by any way out,
 * last registered first, together with __attribute__((cleanup)) variables.
 * Names in the body bind where the defer is written, a return value is
 * computed before the defers run, and a defer not reached yet does not run. */
#pragma yasc
extern int printf(const char * view, ...);

static void note(const char * view s) { printf("%s\n", s); }
static void cl(int * view p) { printf("cleanup %d\n", *p); }

static void t_lifo(void)
{
  defer note("d1");
  int a __attribute__((cleanup(cl))) = 7;
  defer { note("d2"); note("d2b"); }
  note("body");
}

static int t_return_value(void)
{
  int x = 1;
  defer x = 100;
  defer printf("x at defer %d\n", x);
  return x;
}

static int t_early(int n)
{
  defer note("outer");
  if (n > 0)
  {
    defer note("inner");
    if (n > 1)
      return 2;
    note("inner end");
  }
  defer note("late");
  return 0;
}

static void t_loop(void)
{
  for (int i = 0; i < 4; i++)
  {
    defer printf("iter %d done\n", i);
    if (i == 1)
      continue;
    if (i == 3)
      break;
    printf("iter %d\n", i);
  }
  int j = 0;
  while (1)
  {
    defer j++;
    if (j == 2)
      break;
  }
  printf("j %d\n", j);
  int k = 0;
  do
  {
    defer k += 10;
  } while (k < 20);
  printf("k %d\n", k);
}

static void t_goto(void)
{
  int n = 0;
again:
  {
    defer printf("leave %d\n", n);
    n++;
    if (n < 3)
      goto again;
    goto leave;
  }
leave:
  note("out");
  int m = 0;
  {
  top:
    if (m == 2)
      goto done;
    defer printf("m %d\n", m);
    m++;
    goto top;
  }
done:
  note("done");
}

static void t_switch(int v)
{
  switch (v)
  {
  case 1:
  {
    defer note("case 1 out");
    if (v)
      break;
    note("not reached");
  }
  case 2:
    note("case 2");
    break;
  }
}

static void t_binding(void)
{
  int x = 1;
  defer printf("bound x %d\n", x);
  {
    int x = 2;
    printf("inner x %d\n", x);
    return;
  }
}

static int helper_calls;
static inline int with_defer(int a)
{
  defer helper_calls++;
  if (a < 0)
    return -a;
  return a * 2;
}

static void t_nested(void)
{
  defer
  {
    defer note("nested inner");
    note("nested outer");
  }
  int v = ({
    defer note("stmt expr");
    42;
  });
  printf("v %d\n", v);
}

static int t_loop_in_defer(void)
{
  int sum = 0;
  {
    defer
    {
      for (int i = 0; i < 5; i++)
      {
        if (i == 3)
          break;
        sum += i;
      }
    }
  }
  return sum;
}

int main(void)
{
  t_lifo();
  printf("ret %d\n", t_return_value());
  for (int n = 0; n < 3; n++)
    printf("early %d -> %d\n", n, t_early(n));
  t_loop();
  t_goto();
  t_switch(1);
  t_switch(2);
  t_binding();
  int a = with_defer(3);
  int b = with_defer(-4);
  printf("inline %d %d calls %d\n", a, b, helper_calls);
  t_nested();
  printf("loop in defer %d\n", t_loop_in_defer());
  return 0;
}
