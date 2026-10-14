/* libc memset(dst, c, n) takes the fill before the size; __aeabi_memset takes
 * them the other way round.  Passes that read the wrong slot deleted
 * memset(buf, 1, 256) as a one-byte write. */
#include <stdio.h>
#include <string.h>

__attribute__((noinline)) int f(int i)
{
  char buf[256];
  memset(buf, 1, sizeof buf);
  return buf[100] + i;
}

__attribute__((noinline)) void scribble(void)
{
  volatile char junk[512];
  for (int k = 0; k < 512; k++)
    junk[k] = 0x5a;
}

int main(void)
{
  scribble();
  printf("f=%d\n", f(0));
  return 0;
}
