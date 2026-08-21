/* A folded strcpy becomes a BLOCK_COPY over its destination range.  Every pass
 * that forwards a stored value to a later load has to treat that as a killing
 * write, or reads land on bytes the copy has already overwritten. */

char *strcpy(char *d, const char *s);

static int subword(void)
{
  char b[16];
  b[0] = 1; b[1] = 2; b[2] = 3; b[3] = 4; b[4] = 5;
  strcpy(b, "wxyz");                     /* 5 bytes: not a whole word */
  return b[0] + b[1] * 10 + b[4];
}

static int wordsized(void)
{
  char b[16];
  b[0] = 1; b[1] = 2; b[2] = 3; b[3] = 4;
  strcpy(b, "wxy");                      /* 4 bytes */
  return b[0] + b[1] * 10 + b[3];
}

static int partial_overwrite(void)
{
  char b[64];
  strcpy(b, "abcdef");
  b[2] = 0;                              /* truncates after the copy */
  return (int)__builtin_strlen(b);
}

int main(void)
{
  if (subword() != 'w' + 'x' * 10 + 0)
    return 1;
  if (wordsized() != 'w' + 'x' * 10 + 0)
    return 2;
  if (partial_overwrite() != 2)
    return 3;
  return 0;
}
