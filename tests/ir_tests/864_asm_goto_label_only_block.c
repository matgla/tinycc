/* A block reachable only through an `asm goto` label is live: the IR has no
 * edge from the INLINE_ASM to the label, and the reachability sweeps (flat DCE,
 * SSA unreachable) deleted it, so the asm branched into the epilogue of the
 * other return path. */
int printf(const char *, ...);

__attribute__((noinline)) int ret_other(int x)
{
  __asm__ goto("cmp %0, #0\n\tbeq %l1" ::"r"(x) : "cc" : L);
  return 1;
L:
  return 2;
}

__attribute__((noinline)) int with_work(int x)
{
  int r = x * 3;
  __asm__ goto("cmp %0, #7\n\tbeq %l1" ::"r"(x) : "cc" : hit);
  return r;
hit:
  r += 100;
  return r + x;
}

__attribute__((noinline)) int two_labels(int x)
{
  __asm__ goto("cmp %0, #1\n\tbeq %l1\n\tcmp %0, #2\n\tbeq %l2" ::"r"(x) : "cc" : one, two);
  return 0;
one:
  return 10;
two:
  return 20;
}

__attribute__((noinline)) int in_loop(int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
  {
    __asm__ goto("cmp %0, #3\n\tbeq %l1" ::"r"(i) : "cc" : done);
    s += i;
  }
  return s;
done:
  return -s;
}

int main(void)
{
  printf("%d %d\n", ret_other(0), ret_other(5));
  printf("%d %d\n", with_work(7), with_work(2));
  printf("%d %d %d\n", two_labels(1), two_labels(2), two_labels(3));
  printf("%d %d\n", in_loop(10), in_loop(2));
  return 0;
}
