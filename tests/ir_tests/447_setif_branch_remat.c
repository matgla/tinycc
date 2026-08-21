/* Rematerializing a comparison at a distant branch on its SETIF result
 * (source/opt/flat/scalar/branch.c, setif_branch_remat).
 *
 * `int p = (a == K); ... if (p) ...` is compiled by materializing a 0/1 with a
 * cmp/ite/moveq/movne quartet and then testing it against zero at every use.
 * The pass deletes the quartet and redoes the comparison at each branch, which
 * costs the same two instructions the zero-test already cost.  It rewrites
 * branch conditions, so a mistake here is a silently inverted or skipped
 * branch, not a crash -- and the whole ir_tests corpus stayed green while an
 * earlier version of the pass miscompiled `keep_param_redef` below.
 *
 * `keep_param_redef` HAS TEETH: relax the pass's "each non-immediate operand
 * is a TEMP" rule to accept a parameter and this file prints 15 instead of 1 at
 * -O1.  A parameter carries an implicit definition at function entry that
 * tcc_ir_vreg_has_single_def does not count, so one explicit assignment makes
 * two definitions and the redone comparison reads the NEW value.
 *
 * `keep_arith_use` and `keep_two_regs` pin refusals the pass makes for other
 * reasons (the boolean is also used as a number; both operands are registers,
 * which would hand the allocator two live values where it had one bit). */

extern int printf(const char *, ...);

volatile int sink;
volatile int vsrc = 7;

/* The shape the pass is for: set once, branched on twice, far from the set. */
static int remat_twice(int a, int b, int c)
{
  const int p = (a == 0x7FF);
  int r = 0;
  r += c * 3;
  r ^= c << 1;
  if (p)
    r += 100;
  r += b;
  if (p)
    r += 1000;
  return r;
}

/* The operand is reassigned before the branch -- redoing the comparison there
 * would compare the new value. */
static int keep_param_redef(int a, int b)
{
  int p = (a == 5);
  a = b;
  if (p)
    return 1;
  return a;
}

/* The boolean is read as a number, so the 0/1 must still be materialized. */
static int keep_arith_use(int a, int b)
{
  int p = (a > b);
  int s = p * 10;
  if (p)
    s += 3;
  return s;
}

/* Inside a loop: the operand is redefined on every back edge. */
static int remat_in_loop(int n)
{
  int t = 0, i;
  for (i = 0; i < n; i++)
  {
    int p = (i == 3);
    t += i;
    if (p)
      t += 100;
    if (p)
      t += 1000;
  }
  return t;
}

/* A volatile operand: redoing the comparison would be a second access. */
static int keep_volatile(void)
{
  int p = (vsrc == 7);
  sink = 1;
  if (p)
    return 42;
  return 0;
}

/* Two register operands. */
static int keep_two_regs(int a, int b, int c)
{
  int p = (a < b);
  c *= 3;
  if (p)
    c += 7;
  if (p)
    c += 70;
  return c;
}

/* A 64-bit comparison feeding two far branches. */
static int remat_ll(unsigned long long x, unsigned long long y, int k)
{
  int p = (x == y);
  int r = k * 5;
  if (p)
    r += 11;
  r ^= k;
  if (p)
    r -= 3;
  return r;
}

int main(void)
{
  int i;
  for (i = 0; i < 6; i++)
    printf("remat_twice %d\n", remat_twice(i == 2 ? 0x7FF : i, i * 7, i));
  for (i = 0; i < 6; i++)
    printf("keep_param_redef %d\n", keep_param_redef(i, i * 3));
  for (i = 0; i < 6; i++)
    printf("keep_arith_use %d\n", keep_arith_use(i, 3));
  for (i = 0; i < 8; i++)
    printf("remat_in_loop %d\n", remat_in_loop(i));
  printf("keep_volatile %d\n", keep_volatile());
  for (i = 0; i < 6; i++)
    printf("keep_two_regs %d\n", keep_two_regs(i, 3, i));
  for (i = 0; i < 6; i++)
    printf("remat_ll %d\n", remat_ll((unsigned long long)i, 3ULL, i));
  return 0;
}
