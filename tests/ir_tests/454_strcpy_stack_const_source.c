/* strcpy from a frame buffer whose contents are known folds to a copy from the
 * rodata those contents came from.  Each case below is one reason that fold must
 * be refused: the buffer is rewritten before the use, a callee can write through
 * it, or it is itself a copy destination. */

char *strcpy(char *d, const char *s);
unsigned strlen(const char *s);
void *memset(void *d, int c, unsigned n);

static void writer(char *p) { p[2] = 0; }
static void (*volatile pwriter)(char *) = writer;

/* the shape this fold exists for: contents fixed before the loop */
static int folds(int n)
{
  char src[256] = "The quick brown fox jumps over the lazy dog. "
                  "Pack my box with five dozen liquor jugs. "
                  "How vexingly quick daft zebras jump!";
  char dst[256];
  int len = -1;
  for (int i = 0; i < n; i++) { strcpy(dst, src); len = (int)strlen(dst); }
  return len;
}

static int rewritten_in_loop(int n)
{
  char src[64] = "abcdefghij";
  char dst[64];
  int len = -1;
  for (int i = 0; i < n; i++) { src[3] = 0; strcpy(dst, src); len = (int)strlen(dst); }
  return len;
}

static int opaque_writer(int n)
{
  char src[64] = "abcdefghij";
  char dst[64];
  int len = -1;
  for (int i = 0; i < n; i++) { pwriter(src); strcpy(dst, src); len = (int)strlen(dst); }
  return len;
}

static int is_a_destination(int n)
{
  char src[64] = "abcdefghij";
  char dst[64];
  int len = -1;
  for (int i = 0; i < n; i++) { strcpy(src, "xy"); strcpy(dst, src); len = (int)strlen(dst); }
  return len;
}

/* the copy may only be dropped when nothing reads the destination */
static int destination_is_read(int n)
{
  char src[64] = "hello world";
  char dst[64];
  memset(dst, 0, sizeof(dst));
  for (int i = 0; i < n; i++)
    strcpy(dst, src);
  return (dst[0] << 8) | dst[10];
}

int main(void)
{
  if (folds(3) != 122)
    return 1;
  if (folds(0) != -1)                    /* zero trips keeps the pre-loop value */
    return 2;
  if (rewritten_in_loop(3) != 3)
    return 3;
  if (opaque_writer(3) != 2)
    return 4;
  if (is_a_destination(3) != 2)
    return 5;
  if (destination_is_read(3) != (('h' << 8) | 'd'))
    return 6;
  return 0;
}
