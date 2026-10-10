/* ssa:loop_header_dup shapes.  Each loop's exit test sits behind header
 * instructions (or loop_rotate declined the loop), which kept both the
 * top-test branch and an unconditional back-edge `b` in every iteration.
 * After the pass the only backward branch of each loop is the conditional
 * bottom test.
 *   hd_move_bwd -- `while (n-- != 0)`: copy + decrement ahead of the test
 *   hd_find     -- a call in the body (through a pointer: no relocation)
 *   hd_fill     -- the bound is re-read from memory every iteration
 *   hd_outer    -- the outer loop of a nest (its body holds the inner loop) */

void *hd_move_bwd(void *dst, const void *src, unsigned long n)
{
  unsigned char *d = (unsigned char *)dst;
  const unsigned char *s = (const unsigned char *)src;
  while (n-- != 0)
    d[n] = s[n];
  return dst;
}

int hd_find(const int *tab, unsigned n, int key, int (*eq)(int, int))
{
  for (unsigned i = 0; i < n; i++)
    if (eq(tab[i], key))
      return (int)i;
  return -1;
}

struct hd_slice
{
  int *p;
  unsigned len;
};

void hd_fill(struct hd_slice *s, int v)
{
  for (unsigned i = 0; i != s->len; i++)
    s->p[i] = v;
}

int hd_outer(const int *m, int rows, int cols)
{
  int s = 0;
  for (int r = 0; r < rows; r++)
    for (int c = 0; c < cols; c++)
      s += m[r * cols + c] ^ c;
  return s;
}
