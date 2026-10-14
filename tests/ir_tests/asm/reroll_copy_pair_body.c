/* ssa:reroll cost model: an explicitly unrolled copy body (load/store pair
 * plus pointer steps) must stay straight-line - the roll pays ADD+CMP+JUMPIF
 * per element and gives up the post-indexed/folded addressing the body had.
 * A body with real per-element work (add_copies) is long enough to pay for
 * the roll and must still be re-rolled into a counted inner loop.
 */
void postinc_bytes(unsigned char *d, unsigned char *s, unsigned long n) {
  for (; n >= 16; n -= 16) {
    *d++ = *s++;
    *d++ = *s++;
    *d++ = *s++;
    *d++ = *s++;
  }
}

void postinc_words(unsigned *d, unsigned *s, unsigned long n) {
  for (; n >= 16; n -= 16) {
    *d++ = *s++;
    *d++ = *s++;
    *d++ = *s++;
    *d++ = *s++;
  }
}

void fill_bytes(unsigned char *d, unsigned long n) {
  for (; n >= 16; n -= 16) {
    *d++ = 42;
    *d++ = 42;
    *d++ = 42;
    *d++ = 42;
  }
}

void idx_words(unsigned *d, unsigned *s, unsigned long n) {
  for (; n >= 16; n -= 16, d += 4, s += 4) {
    d[0] = s[0];
    d[1] = s[1];
    d[2] = s[2];
    d[3] = s[3];
  }
}

void add_copies(unsigned *d, unsigned *a, unsigned *b, unsigned long n) {
  for (; n >= 16; n -= 16) {
    *d++ = *a++ + *b++;
    *d++ = *a++ + *b++;
    *d++ = *a++ + *b++;
    *d++ = *a++ + *b++;
  }
}
