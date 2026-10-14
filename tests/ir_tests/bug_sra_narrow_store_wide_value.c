/* sra: a narrow member store of a wider value, read back through the member.
 *
 * The frontend stores `q.s = w` (w a word) as a halfword STORE whose value
 * operand carries the STORE's width -- the truncation is the store's own
 * (vstore's delayed char/short cast).  sra took that operand type as proof
 * that the value already fits the field, made the store a copy into the
 * field's VAR and the read a copy out of it: `return q.s` returned the whole
 * word.  Whether a value fits has to come from its definition (a mask, a
 * narrow unsigned load), as in sl_forward's sl_fwd_value_within_bits.
 * Every line below printed the untruncated word at -O1 and above. */
#include <stdio.h>

typedef struct { unsigned short s; unsigned char c, d; unsigned v; } Q;
typedef struct { unsigned char b; signed char sb; short ss; } B;
typedef union { unsigned w; unsigned short h[2]; } U;

volatile int one = 1;
static unsigned __attribute__((noinline)) x(void) { return 0x12345678u; }
static unsigned long long __attribute__((noinline)) k(void) { return 0xdeadbeef3412beefULL; }
static int __attribute__((noinline)) ix(void) { return -0x1234567; }

/* The store and the read in one block, the field promoted. */
static unsigned __attribute__((noinline)) same_block(int c)
{
  Q q;
  unsigned w = x();
  q.s = w;
  q.c = 1;
  if (c)
    q.c = 5;
  return q.s + q.c;
}

/* Two stores reaching one read. */
static unsigned __attribute__((noinline)) join(int c)
{
  Q q;
  q.c = 0;
  unsigned w = x();
  if (c)
    q.s = w;
  else
    q.s = 7;
  return q.s;
}

/* A 64-bit value, implicitly and explicitly narrowed. */
static unsigned __attribute__((noinline)) join64(int c)
{
  Q q;
  q.c = 0;
  unsigned long long w = k();
  if (c)
    q.s = w;
  else
    q.s = 7;
  return q.s;
}

static unsigned __attribute__((noinline)) join64_cast(int c)
{
  Q q;
  q.c = 0;
  unsigned long long w = k();
  if (c)
    q.s = (unsigned)w;
  else
    q.s = 7;
  return q.s;
}

/* A constant-result call: its 64-bit constant reaches the store. */
static unsigned __attribute__((noinline)) const64(void)
{
  Q q;
  unsigned long long w = k();
  q.s = (unsigned)w;
  q.c = (unsigned)w >> 16;
  return q.s + (q.c << 16);
}

/* Byte fields, signed fields. */
static int __attribute__((noinline)) bytes(int c)
{
  B b;
  int v = ix();
  b.ss = 0;
  if (c)
  {
    b.b = v;
    b.sb = v >> 8;
    b.ss = v >> 4;
  }
  else
  {
    b.b = 1;
    b.sb = 2;
  }
  return b.b * 1000000 + b.sb * 1000 + b.ss;
}

/* A halfword of a word field. */
static unsigned __attribute__((noinline)) part(int c)
{
  U u;
  u.w = 0;
  unsigned w = x();
  if (c)
    u.h[1] = w;
  return u.w;
}

int main(void)
{
  printf("%x %x\n", same_block(one), join(one));
  printf("%x %x %x\n", join64(one), join64_cast(one), const64());
  printf("%d %x\n", bytes(one), part(one));
  return 0;
}
