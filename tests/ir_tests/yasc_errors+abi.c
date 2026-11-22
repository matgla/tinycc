/* Plain C (no yasc) calling a yasc function returning !int: an unsigned long
 * long, the value in the low word, the error in the high word (0 = none). */
#include <stdio.h>

extern unsigned long long yasc_parse(const char *s);

void abi_check(void)
{
  unsigned long long r = yasc_parse("42");
  printf("abi ok %u err %u\n", (unsigned)r, (unsigned)(r >> 32));
  r = yasc_parse("4x");
  printf("abi err %u\n", (unsigned)(r >> 32));
}
