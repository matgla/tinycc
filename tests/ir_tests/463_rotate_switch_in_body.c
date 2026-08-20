/* Loop rotation relocates the body to a different set of IR indices.  A
 * SWITCH_TABLE's arm indices live outside the instruction stream, in
 * ir->switch_tables[], so the body-branch remap never saw them: an inlined
 * `switch` inside a rotated body dispatched to whatever now occupied the old
 * slot, and the arms nothing pointed at any more were deleted as dead.
 *
 * The shape needs all three parts together -- a dense switch in the body, a
 * returning call in the same body (which is what makes the loop a rotation
 * candidate at all now), and an entry guard that folds -- so a plain switch
 * test does not reach it.  Every arm returns a distinct value and every case
 * is exercised, including the out-of-range ones, so a misdispatched arm shows
 * up as a wrong number rather than as a crash.
 */
#include <stdio.h>

volatile int sink;

static int pick(int i)
{
  switch (i) {
    case 0: return 70;
    case 1: return 71;
    case 2: return 72;
    case 3: return 73;
    case 4: return 74;
    default: return -70;
  }
}

/* min_val != 0, and arms that fall through into their successor. */
static int pick_off(int i)
{
  int r = 0;
  switch (i) {
    case 3: r += 1; /* fall through */
    case 4: r += 2; /* fall through */
    case 5: r += 4; break;
    case 6: r += 8; break;
    default: r = 900; break;
  }
  return r;
}

int main(void)
{
  int sum = 0;

  /* The rotation candidate: constant-folding entry guard (-2 < 8), a call in
   * the body, and two switch dispatches inside it. */
  for (int i = -2; i < 8; i++) {
    int a = pick(i);
    int b = pick_off(i);
    sum += a * 3 + b;
    printf("i=%d pick=%d off=%d\n", i, a, b);
  }

  /* Same body reached with a runtime bound, so the guard does NOT fold and
   * the loop keeps its top test: both layouts must agree. */
  int n = (int)(sink + 4);
  for (int i = 0; i < n; i++)
    sum += pick(i) + pick_off(i);

  printf("sum=%d\n", sum);
  return 0;
}
