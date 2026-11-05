/* At -O0 no value stays in a register across control flow (ra_o0_direct): there is
 * no liveness to protect one.  Each function below has a value that reaches a use
 * by a way a register's range would miss: defined on some paths only, carried round
 * a back edge, redefined conditionally in a loop, leaving by a forward jump and
 * returning by a backward one.  Every level must print the same. */
#include <stdio.h>

typedef long long i64;

__attribute__((noinline)) int use(int x) { return x + 1; }

/* Only some iterations define v; the others read the previous one. */
__attribute__((noinline)) int conditional_def(int n)
{
  int v = 100, s = 0;
  for (int i = 0; i < n; i++)
  {
    if (i & 1)
      v = i * 3;
    s += use(v);
  }
  return s;
}

/* Defined only inside the loop, on some iterations, and read after the inner loop:
 * the references are all inside the outer loop, so the range does not reach its
 * back edge, and another value can be given the register in between. */
__attribute__((noinline)) int index_of_max(int *x, int n)
{
  int rounds = 0, total = 0, mi;
  for (;;)
  {
    int max = 0;
    for (int i = 0; i < n; i++)
    {
      int candidate = x[i] + rounds;
      int weight = candidate * 3 - rounds;
      if (candidate > max && weight > 0)
      {
        max = candidate;
        mi = i;
      }
    }
    if (max == rounds)
      return total;
    total += mi * 100 + max;
    x[mi] = 0;
    rounds = 0;
  }
}

/* A flag set in one arm and read on a later pass round the loop. */
__attribute__((noinline)) int flag_across_iterations(const char *s)
{
  int seen, count = 0, i = 0;
  for (;;)
  {
    char c = s[i++];
    int code = c * 2 + count;
    if (!c)
      return count;
    if (c == 'a')
      seen = code;
    else if (i > 1 && seen > code - 200)
      count += seen & 15;
    else
      count += use(code) & 1;
  }
}

/* Every way into a block must count as a block start, or what is in a register at
 * one of them stays there: a value made above each kind of join and read below it.
 * The arms clobber registers with calls and with a value of their own. */
__attribute__((noinline)) int join_after_branch(int a, int b)
{
  int v = a * 3 + 1;
  if (b)
  {
    int w = use(b) * 5;
    v = w + b;
  }
  int r = use(a) + a;
  return v * 2 + r;
}

__attribute__((noinline)) int join_after_switch(int k, int a)
{
  int v = a + 7, u = a * 2;
  switch (k)
  {
  case 0: v = use(u) + 1; break;
  case 1: u = use(v) + 2; break;
  case 2: v = u + v; /* fall through */
  case 3: u = v - use(k); break;
  case 4: return v + 1;
  default: v = k * 9;
  }
  return v * 100 + u;
}

__attribute__((noinline)) int back_to_a_join(int n)
{
  int v = n + 2, i = 0;
again:
  i += use(v) & 3;
  if (i < 9)
  {
    int w = i * i;
    v = v + w - use(w);
    goto again;
  }
  return v + i;
}

/* The loop head is the first instruction, so a parameter's range starts on it: it
 * still comes from before the loop, and another value must not take its register
 * for the rest of the body. */
__attribute__((noinline)) void loop_head_at_entry(int *d, float *x, float *y, int n)
{
  while (n--)
    *d++ = *x++ == *y++;
}

/* A parameter redefined in a loop. */
__attribute__((noinline)) int param_redefined(int p, int n)
{
  int s = 0;
  while (n-- > 0)
  {
    s += p;
    if (s & 2)
      p = s + n;
  }
  return s + p;
}

/* A value leaves by a forward jump and comes back by a backward one. */
__attribute__((noinline)) int forward_then_back(int n)
{
  int a = n * 7, b = 0, steps = 0;
  goto middle;
top:
  b += a;
  if (steps > 4)
    return a + b;
middle:
  steps = steps + use(steps) - steps;
  a = a - 1 + (steps & 1);
  goto top;
}

/* A statement expression holds a temporary across a loop inside an expression. */
__attribute__((noinline)) int stmt_expr(int n)
{
  int s = 3 + n * 2 + ({
    int t = 0;
    for (int i = 0; i < n; i++)
      t += use(i);
    t;
  });
  return s * (n + ({ int k = n; if (k > 2) k = 2; k; }));
}

/* Conditional expressions and && || make a value with a definition on each arm. */
__attribute__((noinline)) int two_arms(int a, int b)
{
  int x = (a > b ? a * 2 : b * 3) + (a && b ? use(a) : use(b)) + (a || !b);
  return x + (a < b ? (b > 5 ? 1 : 2) : (a > 5 ? 3 : 4));
}

/* Wide and floating values across blocks, and across calls. */
__attribute__((noinline)) i64 wide(i64 v, int n)
{
  i64 acc = v;
  for (int i = 0; i < n; i++)
  {
    if (i % 3)
      acc = acc * 3 + i;
    acc ^= use(i);
  }
  return acc;
}

__attribute__((noinline)) int floats(float f, double d, int n)
{
  for (int i = 0; i < n; i++)
  {
    if (i & 1)
      f = f + 1.5f;
    else
      d = d * 2.0;
    use(i);
  }
  return (int)(f * 2) + (int)d;
}

/* A switch whose arms read what is defined above and redefine it. */
__attribute__((noinline)) int switched(int k, int n)
{
  int v = k, s = 0;
  for (int i = 0; i < n; i++)
  {
    switch ((v + i) & 3)
    {
    case 0:
      v += 5;
      break;
    case 1:
      s += v;
      /* fall through */
    case 2:
      v = v * 2 + 1;
      break;
    default:
      s -= use(v);
    }
  }
  return s + v;
}

/* The address goes through a pointer variable: the object must keep its slot
 * while the pointer is live, though nothing names the object any more. */
__attribute__((noinline)) int through_pointer(int c)
{
  int a = 11, b = 22, *p, v = 0;
  p = c ? &a : &b;
  for (int i = 0; i < 3; i++)
  {
    int t = use(i) + i;
    v += *p + t;
    *p = t * 2;
  }
  return v + a + b;
}

/* Computed goto: a block can be reached from anywhere. */
__attribute__((noinline)) int computed(int n)
{
  static void *const next[] = {&&one, &&two, &&done};
  int s = n, i = 0;
  goto *next[0];
one:
  s += use(i);
  i++;
  goto *next[i & 1 ? 1 : (i > 4 ? 2 : 0)];
two:
  s *= 2;
  i++;
  goto *next[i > 4 ? 2 : 0];
done:
  return s;
}

int main(void)
{
  float fa[] = {5, 1, 3, 5}, fb[] = {2, 4, 3, 0};
  int out[4];
  loop_head_at_entry(out, fa, fb, 4);
  printf("%d %d %d %d\n", out[0], out[1], out[2], out[3]);
  printf("%d\n", conditional_def(9));
  int xs[] = {3, 9, 1, 7, 2};
  printf("%d\n", index_of_max(xs, 5));
  printf("%d\n", flag_across_iterations("abacaxbbyya"));
  printf("%d %d\n", join_after_branch(4, 0), join_after_branch(4, 3));
  for (int k = 0; k < 6; k++)
    printf("%d ", join_after_switch(k, 11));
  printf("\n%d\n", back_to_a_join(5));
  printf("%d\n", param_redefined(5, 9));
  printf("%d\n", forward_then_back(3));
  printf("%d\n", stmt_expr(4));
  printf("%d %d %d\n", two_arms(1, 2), two_arms(7, 0), two_arms(0, 6));
  printf("%lld\n", wide(0x123456789LL, 7));
  printf("%d\n", floats(1.5f, 1.25, 5));
  printf("%d\n", switched(3, 9));
  printf("%d %d\n", through_pointer(0), through_pointer(1));
  printf("%d\n", computed(1));
  return 0;
}
