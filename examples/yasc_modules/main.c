/* A yasc program using the modules: `import`, not #include.  Macros never
 * cross an import, in either direction. */
#include <stdio.h>

import shapes; /* brings lib.vec with it */

int main(void)
{
  rect_t r = rect_make(0, 0, 4, 3);
  vec2_t by = {2, 1};
  rect_t m = rect_move(r, by);

  printf("area %d width %d\n", rect_area(m), rect_width(m));
  printf("moved to %d,%d\n", vec_get(m.lo, AXIS_X), vec_get(m.lo, AXIS_Y));
  printf("dot %d\n", vec_dot(m.lo, m.hi));
  printf("vector ops %d\n", vec_ops);
  return 0;
}
