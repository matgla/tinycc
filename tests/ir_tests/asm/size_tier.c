/* -Os / -Oz: the -O2 pipeline where a choice trades code size for speed
 * taken the small way (test_size_tier in test_codegen_asm.py). */

int size_macro(void)
{
#ifdef __OPTIMIZE_SIZE__
  return 1;
#else
  return 0;
#endif
}

/* 160 bytes: larger than the largest copy stub (32 words). */
struct big
{
  int w[40];
};

void copy_big(struct big *d, const struct big *s)
{
  *d = *s;
}

/* Loops whose heads -O2 word-aligns with a NOP when they land at 2 mod 4. */
int sum_pos(const int *p, int n)
{
  int s = 0;
  for (int i = 0; i < n; i++)
    if (p[i] > 3)
      s += p[i];
    else
      s -= 1;
  return s;
}

int count_eq(const short *p, int n, int k)
{
  int c = 0;
  short x = (short)k;
  for (int i = 0; i < n; i++)
    c += p[i] == x;
  return c;
}

unsigned mix(const unsigned char *p, int n)
{
  unsigned h = 7;
  while (n-- > 0)
    h = h * 31 + *p++;
  return h;
}

/* A function that takes a label's address never widens an instruction to
 * align a loop head (fix/executed-nops), so its -O2 pad stays a NOP: the
 * loops above may all align by widening, which leaves no NOP to count. */
unsigned mix_label_addr(const unsigned char *p, int n)
{
  static void *const out[] = {&&done};
  unsigned h = 7;
  while (n-- > 0)
    h = h * 31 + *p++;
  goto *out[0];
done:
  return h;
}
