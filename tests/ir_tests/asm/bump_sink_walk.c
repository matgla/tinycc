/* `*p++ = v` / `*d++ = *s++`: the increment is sunk below the access, so the
 * loop is a post-indexed access with no copy of the pointer. */
void fill(unsigned char *p, unsigned char b, unsigned n)
{
  for (; n; --n)
    *p++ = b;
}

void copy(unsigned char *d, const unsigned char *s, unsigned n)
{
  for (; n; --n)
    *d++ = *s++;
}

void fill_words(unsigned *p, unsigned w, unsigned n)
{
  for (; n >= 4; n -= 4)
    *p++ = w;
}
