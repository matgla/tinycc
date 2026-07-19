/* stack_addr_nonnull_fold must not treat two distinct locals as the same
 * address.  It identified a tracked stack address by irop_get_stack_offset,
 * but a scalar local reaches the pass as `&V` whose frame offset is still
 * unassigned pre-RA -- it reads 0 for EVERY distinct VAR.  So `&c` and `&d`
 * both keyed as offset 0, compared "equal", and the pass folded the CMP with
 * evaluate_compare_condition(0, 0, tok): `&c == &d` became TRUE at -O2 while
 * -O0 got it right.  Fixed by keying identity on the (VAR position, offset)
 * pair, with a separate kind for local_stack slots (aggregates), whose
 * parse-time offset IS a real allocated byte and stays a sound identity.
 *
 * Shape matters: the pass folds CMP+JUMPIF, so each check must be an `if`
 * with a call in both arms (returning the comparison instead if-converts to
 * SETIF/ite and never reaches the fold).  `sink` is called through a volatile
 * function pointer so it stays an opaque CALL between the LEAs and the CMP --
 * inlining it reshapes the IR and hides the bug.  vv is volatile so main's
 * body cannot be const-folded away.
 *
 * Reference (arm-none-eabi-gcc -O2): matches tcc -O0/-O1/-O2/-Os.
 */
#include <stdio.h>

volatile int vv = 1;
volatile char *g_sink;

static void sink_impl(char *p, char *q)
{
  g_sink = p;
  g_sink = q;
}

static void (*volatile sink)(char *, char *) = sink_impl;

static void report(const char *name, int v)
{
  printf("%s=%d\n", name, v);
}

/* Two distinct scalar locals: both reach the fold as `&V` with offset 0. */
static void distinct_vars(void)
{
  char c;
  char d;
  sink(&c, &d);
  if (&c == &d)
    report("distinct_vars", 1);
  else
    report("distinct_vars", 0);
}

/* Same VAR, same delta: still soundly foldable to true. */
static void same_var(void)
{
  char c;
  sink(&c, &c);
  if (&c == &c)
    report("same_var", 1);
  else
    report("same_var", 0);
}

/* Aggregate in local_stack: the parse-time offsets differ and are real. */
static void distinct_slots(void)
{
  char buf[8];
  sink(&buf[0], &buf[4]);
  if (&buf[0] == &buf[4])
    report("distinct_slots", 1);
  else
    report("distinct_slots", 0);
}

static void same_slot(void)
{
  char buf[8];
  sink(&buf[0], &buf[0]);
  if (&buf[0] == &buf[0])
    report("same_slot", 1);
  else
    report("same_slot", 0);
}

/* The pass's sound core: a stack address is never null. */
static void var_nonnull(void)
{
  int a;
  sink((char *)&a, (char *)&a);
  if (&a != 0)
    report("var_nonnull", 1);
  else
    report("var_nonnull", 0);
}

int main(void)
{
  if (vv)
    {
      distinct_vars();
      same_var();
      distinct_slots();
      same_slot();
      var_nonnull();
    }
  return 0;
}
