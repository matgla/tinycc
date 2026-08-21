/* ssa:switch_fold picks the arm of a switch whose selector folded to a
 * constant.  The selector reaching it is the index the frontend already
 * normalized (`value - min_val`, gen/stmt/switch.c), not the raw case value;
 * subtracting min_val a second time landed min_val arms too far along.
 *
 * Invisible for the usual `case 0:` table -- min_val is 0 and the two agree --
 * so every switch below starts somewhere else.  `switch (i)` on a constant
 * i == -1 over cases -2..4 used to run `case 1` and leave r == 4.
 */
#include <stdio.h>

volatile int sink;

int main(void)
{
  int i = -1;
  int r = 99;
  switch (i) {
    case -2: r = 33; break;
    case -1: r = 0;  break;
    case 0:  r = 7;  break;
    case 1:  r = 4;  break;
    case 2:  r = 3;  break;
    case 3:  r = 15; break;
    case 4:  r = 9;  break;
  }
  printf("neg min: r = %d\n", r);

  int j = 12;
  int s = 99;
  switch (j) {
    case 10: s = 100; break;
    case 11: s = 110; break;
    case 12: s = 120; break;
    case 13: s = 130; break;
  }
  printf("pos min: s = %d\n", s);

  /* Out of the table's range: the default (here, falling past the switch)
   * must survive the fold too. */
  int k = 99;
  int t = 7;
  switch (k) {
    case 10: t = 100; break;
    case 11: t = 110; break;
    case 12: t = 120; break;
    case 13: t = 130; break;
  }
  printf("out of range: t = %d\n", t);

  /* Same tables with a run-time selector must reach the same arms. */
  sink = -1;
  int v = sink;
  int u = 99;
  switch (v) {
    case -2: u = 33; break;
    case -1: u = 0;  break;
    case 0:  u = 7;  break;
    case 1:  u = 4;  break;
    case 2:  u = 3;  break;
    case 3:  u = 15; break;
    case 4:  u = 9;  break;
  }
  printf("runtime neg min: u = %d\n", u);

  if (r == 0 && s == 120 && t == 7 && u == 0)
    printf("PASS\n");
  else
    printf("FAIL\n");
  return 0;
}
