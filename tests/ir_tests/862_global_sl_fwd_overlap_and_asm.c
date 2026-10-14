/* Global store->load forwarding must drop a remembered value when a later
 * store overlaps it at a different offset/width, or inline asm writes memory. */
int printf(const char *, ...);

int g;
__attribute__((noinline)) int byte_into_word(void)
{
  g = 0x11223344;
  ((char *)&g)[1] = 0;
  return g; /* 0x11220044 */
}

long long q;
__attribute__((noinline)) int word_into_dword(void)
{
  q = 0x1111111122222222LL;
  *(int *)((char *)&q + 4) = 7;
  return (int)(q >> 32); /* 7 */
}

short s2;
__attribute__((noinline)) int short_over_word(void)
{
  g = -1;
  *(short *)&g = 0;
  return g; /* 0xffff0000 */
}

int k;
__attribute__((noinline)) int asm_store(void)
{
  k = 1;
  __asm__ volatile("movs r3, #6\n\tstr r3, [%0]" : : "r"(&k) : "r3", "memory");
  return k; /* 6 */
}

int out;
__attribute__((noinline)) int asm_output(void)
{
  out = 1;
  __asm__ volatile("movs %0, #9" : "=r"(out));
  return out; /* 9 */
}

int main(void)
{
  printf("%08x %d %08x %d %d\n", byte_into_word(), word_into_dword(), (unsigned)short_over_word(), asm_store(),
         asm_output());
  return 0;
}
