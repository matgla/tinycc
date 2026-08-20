/* A folded strcpy must copy exactly strlen+1 bytes.  When that is not a whole
 * number of words the copy has a 1..3 byte tail, and the bytes past the NUL must
 * be left alone.  Every residue class and every LDM chunk boundary is covered. */

char *strcpy(char *d, const char *s);
void *memset(void *d, int c, unsigned n);

#define CHECK(lit, code)                                                       \
  do {                                                                         \
    char b[64];                                                                \
    unsigned len = sizeof(lit) - 1;                                            \
    memset(b, 0x5A, sizeof(b));                                                \
    strcpy(b, lit);                                                            \
    for (unsigned i = 0; i <= len; i++)                                        \
      if (b[i] != (lit)[i])                                                    \
        return (code);                                                         \
    for (unsigned i = len + 1; i < sizeof(b); i++)                             \
      if ((unsigned char)b[i] != 0x5A)                                         \
        return (code) + 1;                                                     \
  } while (0)

int main(void)
{
  CHECK("", 10);
  CHECK("a", 20);
  CHECK("ab", 30);
  CHECK("abc", 40);
  CHECK("abcd", 50);
  CHECK("abcde", 60);
  CHECK("abcdef", 70);
  CHECK("abcdefg", 80);
  CHECK("abcdefghijklmno", 90);
  CHECK("abcdefghijklmnop", 100);
  CHECK("abcdefghijklmnopq", 110);
  CHECK("abcdefghijklmnopqr", 120);
  CHECK("abcdefghijklmnopqrs", 130);
  CHECK("abcdefghijklmnopqrst", 140);
  CHECK("abcdefghijklmnopqrstu", 150);
  CHECK("abcdefghijklmnopqrstuv", 160);
  CHECK("abcdefghijklmnopqrstuvwxyz", 170);
  CHECK("abcdefghijklmnopqrstuvwxyz0123", 180);
  return 0;
}
