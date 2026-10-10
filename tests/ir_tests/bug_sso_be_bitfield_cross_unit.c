#include <stdio.h>

/* PCC layout demotes `long long :4` to int, so f1 (bits 29..32) crosses the
   32-bit unit: the big-endian bit position used to go negative, and the
   widened 8-byte unit must still be stored big-endian. */
typedef struct __attribute__((scalar_storage_order("big-endian"))) {
  unsigned long long f0 : 29;
  unsigned long long f1 : 4;
  unsigned long long f2 : 31;
} S1;

int main(void) {
  S1 v;
  unsigned char *p = (unsigned char *)&v;
  int i;
  for (i = 0; i < (int)sizeof v; i++)
    p[i] = 0;
  v.f0 = 23;
  v.f1 = 5;
  v.f2 = 0x12345;
  printf("size %d\n", (int)sizeof v);
  printf("%u %u %u\n", (unsigned)v.f0, (unsigned)v.f1, (unsigned)v.f2);
  for (i = 0; i < (int)sizeof v; i++)
    printf(" %02x", p[i]);
  printf("\n");
  return 0;
}
