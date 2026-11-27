/* test_yasc_unreachable.py: yasc's `unreachable` and __builtin_unreachable()
 * at run time.  argv[1] names the case; a case prints "ok <case> <value>" or
 * traps (`udf #254`, a HardFault on the QEMU board), or in handler mode
 * reports through __ubsan_handle_builtin_unreachable and aborts. */
#pragma yasc
#include <stdio.h>
#include <string.h>

enum color { RED, GREEN, BLUE };

error E { Bad };

static volatile int v_three = 3, v_four = 4, v_zero, v_one = 1;

static int name_len(enum color c)
{
  switch (c)
  {
  case RED:
    return 3;
  case GREEN:
    return 5;
  case BLUE:
    return 4;
  default:
    unreachable; /* LINE switch */
  }
}

static int half(int x)
{
  if (x % 2 == 0)
    return x / 2;
  unreachable; /* LINE half */
}

static !int get(int x)
{
  if (x)
    return error.Bad;
  return 7;
}

static int pick(int x)
{
  return get(x) catch unreachable; /* LINE catch */
}

static int loop_find(int want)
{
  for (int i = 0; i < 4; i++)
    if (i == want)
      return i * 10;
  unreachable; /* LINE loop */
}

/* inlined into the caller at -O2: the trap stays on the failing side */
static inline int checked_div(int a, int b)
{
  if (b == 0)
    unreachable; /* LINE inline */
  return a / b;
}

static int builtin(int x)
{
  if (x > 1)
    return x;
  __builtin_unreachable(); /* LINE builtin */
}

int main(int argc, char *view *argv)
{
  const char *view c = argc > 1 ? argv[1] : "none";
  int r = 0;
  printf("go %s\n", c);
  if (!strcmp(c, "switch_ok"))
    r = name_len((enum color)v_one);
  else if (!strcmp(c, "switch"))
    r = name_len((enum color)(v_four + 3));
  else if (!strcmp(c, "half_ok"))
    r = half(v_four);
  else if (!strcmp(c, "half"))
    r = half(v_three);
  else if (!strcmp(c, "catch_ok"))
    r = pick(v_zero);
  else if (!strcmp(c, "catch"))
    r = pick(v_one);
  else if (!strcmp(c, "loop_ok"))
    r = loop_find(v_three);
  else if (!strcmp(c, "loop"))
    r = loop_find(v_four);
  else if (!strcmp(c, "inline_ok"))
    r = checked_div(v_four, v_one);
  else if (!strcmp(c, "inline"))
    r = checked_div(v_four, v_zero);
  else if (!strcmp(c, "builtin_ok"))
    r = builtin(v_four);
  else if (!strcmp(c, "builtin"))
    r = builtin(v_one); /* only -fsanitize=unreachable says anything */
  printf("ok %s %d\n", c, r);
  return 0;
}
