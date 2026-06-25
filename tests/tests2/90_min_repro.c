/* Minimal standalone extract of test_init_struct_from_struct from
 * 90_struct-init.c, for on-device debugging of the HW-only c[1].y=5
 * miscompile. Baked into the rootfs at /usr/90_min_repro.c by build_rootfs.sh.
 *
 * Expected: test_init_struct_from_struct: 1 2 3 4 - 1 2 3 4 - 3 4 5 6
 * HW (on-device tcc -O0): ... 1 2 3 5 ...  if it reproduces standalone.
 */
#include <stdio.h>

void test_init_struct_from_struct(void)
{
  int i = 0;
  struct S
  {
    int x, y;
  } a = {1, 2}, b = {3, 4}, c[] = {a, b}, d[] = {++i, ++i, ++i, ++i},
    e[] = {b, (struct S){5, 6}};

  printf("%s: %d %d %d %d - %d %d %d %d - %d %d %d %d\n", __FUNCTION__, c[0].x,
         c[0].y, c[1].x, c[1].y, d[0].x, d[0].y, d[1].x, d[1].y, e[0].x, e[0].y,
         e[1].x, e[1].y);
}

int main(void)
{
  test_init_struct_from_struct();
  return 0;
}
