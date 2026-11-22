/* yasc modules: imports `shapes`, which brings `geom` with it.  The macro below
 * shares a name with a member of geom's interface: an interface is replayed
 * without macro expansion, so the importer's macros never change it. */
#pragma yasc
#define x 999
import shapes;
import geom; /* a second import of a module already loaded: nothing happens */
#undef x
#include <stdio.h>

int main(void)
{
  rect_t r = {{0, 0}, {3, 4}};
  point_t d = {1, -2};
  rect_t s = rect_shift(r, d);
  printf("area %d\n", rect_area(s));
  printf("lo %d %d hi %d %d\n", s.lo.x, s.lo.y, s.hi.x, s.hi.y);
  printf("manhattan %d\n", manhattan(d));
  printf("size %d\n", (int)point_size());
  printf("dirs %d %d %d\n", NORTH, EAST, rect_side());
  printf("moves %d calls %d\n", moves, geom_calls());
  printf("cells %d\n", grid_cells(grid_get()));
  return 0;
}
