/* The same modules called from plain C, through the headers that
 * -fmodule-header=include wrote when they were compiled.  Build it with
 * -ccompat (regular C) and -Iinclude; any C99 compiler takes the headers. */
#include <stdio.h>
#include <shapes.h> /* includes <lib/vec.h> */

int main(void)
{
  rect_t r = rect_make(1, 1, 3, 5);
  vec2_t a = {3, 4};

  printf("area %d width %d\n", rect_area(r), rect_width(r));
  printf("dot %d\n", vec_dot(a, a)); /* export inline: the module's definition */
  printf("vector ops %d\n", vec_ops);
  return 0;
}
