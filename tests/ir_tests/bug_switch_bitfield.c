/* Regression test: switch on bitfield value.
 * Before the fix, the switch table used the full containing word
 * instead of the extracted bitfield value, causing a wild jump. */
#include <stdio.h>

struct packed_flags
{
  unsigned int pad : 25;
  unsigned int btype : 3;
  unsigned int extra : 4;
};

const char *btype_name(struct packed_flags f)
{
  switch (f.btype)
  {
  case 0:
    return "INT32";
  case 1:
    return "INT64";
  case 2:
    return "FLOAT32";
  case 3:
    return "FLOAT64";
  case 4:
    return "STRUCT";
  case 5:
    return "FUNC";
  case 6:
    return "INT8";
  case 7:
    return "INT16";
  default:
    return "UNKNOWN";
  }
}

int main(void)
{
  struct packed_flags f;
  f.pad = 0x1FFFFFF;
  f.extra = 0xF;
  for (int i = 0; i < 8; i++)
  {
    f.btype = i;
    printf("%d: %s\n", i, btype_name(f));
  }
  return 0;
}
