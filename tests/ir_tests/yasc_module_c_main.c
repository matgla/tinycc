/* yasc modules called from plain C: geom.h and shapes.h are the C headers
 * tcc -fmodule-header= wrote when the modules were compiled
 * (test_qemu.py test_yasc_modules_c_caller).  No #pragma yasc here. */
#include <stdio.h>
#include <shapes.h> /* includes <geom.h> */

int main(void)
{
  rect_t r = {{0, 0}, {3, 4}};
  point_t d = {1, -2};
  rect_t s = rect_shift(r, d);
  printf("area %d\n", rect_area(s));
  printf("manhattan %d\n", manhattan(d)); /* export inline: the module's definition */
  printf("side %d\n", rect_side());       /* export static inline: the header's body */
  printf("moves %d calls %d\n", moves, geom_calls());
  printf("cells %d\n", grid_cells(grid_get()));
  return 0;
}
