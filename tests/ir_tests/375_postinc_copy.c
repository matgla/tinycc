/* Post-index writeback addressing (opt_postinc_fusion, -O2): an int array
 * copy loop gets both a LOAD_POSTINC (src) and a STORE_POSTINC (dst) — two
 * independent writeback pointers bumped once per iteration. */
#include <stdio.h>

int src[24], dst[24];

int main(void)
{
  for (int i = 0; i < 24; i++)
    src[i] = i * 5 + 3;

  for (int i = 0; i < 24; i++)
    dst[i] = src[i];

  int s = 0;
  for (int i = 0; i < 24; i++)
    s = s * 31 + dst[i];

  printf("s=%d\n", s);
  return 0;
}
