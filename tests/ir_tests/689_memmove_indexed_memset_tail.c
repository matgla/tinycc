/* memmove_to_indexed_stores folds `*dst = tmp` (memcpy from a frame
   temporary) into the stores that built tmp, retargeted at dst.  A compound
   literal is built as memset(tmp, 0) plus the explicit members; the memset is
   moved onto a stack destination but, through a pointer, stayed on the dead
   temporary -- so a trailing `0` member and the padding were never written:
   `dead` read back as whatever the heap held. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef struct { int kind, obj, lo, hi; int i; int copy; int pool; int temp; int base; uint8_t dead; } Acc;
typedef struct { int pad[7]; Acc *acc; int nacc, cacc; } D;
__attribute__((noinline)) Acc *add(D *d, int kind, int o, int lo, int hi, int i, int copy)
{
  if (d->nacc == d->cacc)
  {
    d->cacc = d->cacc ? 2 * d->cacc : 4;
    d->acc = realloc(d->acc, sizeof(Acc) * d->cacc);
    memset(d->acc + d->nacc, 0xAB, sizeof(Acc) * (d->cacc - d->nacc));
  }
  d->acc[d->nacc] = (Acc){kind, o, lo, hi, i, copy, -1, -1, -1, 0};
  return &d->acc[d->nacc++];
}
int main(void)
{
  D d = {0};
  for (int k = 0; k < 6; k++)
    add(&d, k, 1, 2, 3, 4, 5);
  int bad = 0;
  for (int k = 0; k < 6; k++)
    bad += d.acc[k].dead != 0 || d.acc[k].base != -1 || d.acc[k].kind != k;
  printf("bad %d dead0 %d\n", bad, d.acc[0].dead);
  return 0;
}
