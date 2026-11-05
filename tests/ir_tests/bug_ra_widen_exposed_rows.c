/* ra_widen_intervals_by_liveness gives a liveness row only to a vreg read before
 * its block defines it.  Each function here has a value that looks local to one
 * block but is live around a back edge or across a block boundary, so dropping
 * its row would let a register be reused while the value is still needed. */

__attribute__((noinline)) int sink(int a, int b, int c, int d) { return a + 2 * b + 3 * c + 4 * d; }

/* Single-block loop: `prev` is read at the top of the block that defines it. */
__attribute__((noinline)) int read_before_def_in_loop(int n)
{
  int prev = 1, sum = 0;
  for (int i = 0; i < n; i++)
  {
    sum += prev * 3;
    prev = sum ^ i;
  }
  return sum + prev;
}

/* Defined in one block, read only as a call argument in a later one: the read is
 * a parameter instruction, found through the call's parameter list. */
__attribute__((noinline)) int param_read_in_later_block(int x, int n)
{
  int total = 0;
  for (int i = 0; i < n; i++)
  {
    int a = x + i, b = x - i, c = x * i, d = x ^ i;
    if (i & 1)
      total += sink(a, b, c, d);
    else
      total -= sink(d, c, b, a);
  }
  return total;
}

/* Defined on one path only, so also live through the other (the join reads it
 * before any definition reaches it on that path). */
__attribute__((noinline)) int defined_on_one_path(int x, int n)
{
  int v = x, out = 0;
  for (int i = 0; i < n; i++)
  {
    if (i % 3 == 0)
      v = out + i;
    int t0 = v * 2, t1 = t0 + 1, t2 = t1 ^ i;
    out += t2 + sink(t0, t1, t2, v);
  }
  return out;
}

/* A parameter read only inside the loop, never redefined: live from entry. */
__attribute__((noinline)) int param_live_around_loop(int k, int m, int n)
{
  int r = 0;
  for (int i = 0; i < n; i++)
  {
    int t = i * i;
    r += sink(t, t + 1, t + 2, t + 3);
    if (r & 1)
      r += k;
    else
      r -= m;
  }
  return r;
}

/* The same shape with many temporaries that are local to their block, so most
 * of the function has no row at all. */
__attribute__((noinline)) int many_local_temps(int x, int n)
{
  int acc = 0;
  for (int i = 0; i < n; i++)
  {
    x += acc;
    if (x & 1) { int a = x + 1, b = a * 3, c = b ^ a; acc += c + b; }
    if (x & 2) { int a = x + 2, b = a * 5, c = b ^ a; acc += c - b; }
    if (x & 4) { int a = x + 3, b = a * 7, c = b ^ a; acc += c + a; }
    if (x & 8) { int a = x + 4, b = a * 9, c = b ^ a; acc += sink(a, b, c, acc); }
  }
  return acc + x;
}

/* The two below are read above their definition only through the back edge, and
 * every linear reference ends before the loop's tail, which fills the registers.
 * `s` is read by the instruction that redefines it; nothing else reads it first. */
__attribute__((noinline)) int read_by_own_def(volatile int *p, int n)
{
  int s = 1, acc = 0;
  for (int i = 0; i < n; i++)
  {
    s = s + p[0];
    acc ^= sink(s, 1, 2, 3);
    int t0 = p[1] * 3, t1 = p[2] * 5, t2 = p[3] * 7, t3 = t0 + t1, t4 = t1 ^ t2, t5 = t3 - t2, t6 = t4 + t5, t7 = t6 * t0;
    acc += t0 ^ t1 ^ t2 ^ t3 ^ t4 ^ t5 ^ t6 ^ t7;
  }
  return acc;
}

/* `v` is read only as a call argument at the top of the loop. */
__attribute__((noinline)) int read_only_as_argument(volatile int *p, int n)
{
  int v = p[0], acc = 0;
  for (int i = 0; i < n; i++)
  {
    acc ^= sink(v, i, 2, 3);
    v = p[1] + i;
    int t0 = p[1] * 3, t1 = p[2] * 5, t2 = p[3] * 7, t3 = t0 + t1, t4 = t1 ^ t2, t5 = t3 - t2, t6 = t4 + t5, t7 = t6 * t0;
    acc += t0 ^ t1 ^ t2 ^ t3 ^ t4 ^ t5 ^ t6 ^ t7;
  }
  return acc;
}

int main(void)
{
  if (read_before_def_in_loop(7) != 25176) return 1;
  if (param_read_in_later_block(5, 9) != (-92)) return 2;
  if (defined_on_one_path(3, 10) != 1476128) return 3;
  if (param_live_around_loop(11, 13, 8) != 1552) return 4;
  volatile int m[4] = {3, 5, 7, 11};
  if (read_by_own_def(m, 9) != (-10722)) return 6;
  if (read_only_as_argument(m, 9) != (-10741)) return 7;
  if (many_local_temps(1, 12) != (-1557015927)) return 5;
  return 0;
}
