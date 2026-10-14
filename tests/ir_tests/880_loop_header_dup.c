/* ssa:loop_header_dup -- bottom-testing of `while` loops whose exit test sits
 * behind a few header instructions (`n-- != 0`, `i++ < n`, the CBE
 * `v = i; if (!(v < n)) break;` copies).  The header is duplicated in front of
 * the back-edge, which becomes the inverted conditional branch.
 *
 * Nothing checks the SHAPE here (test_codegen_asm.py does); these are the ways
 * a header copy can silently go wrong:
 *
 *   mv_fwd / mv_bwd   -- `while (n-- != 0)`: the decrement lives in the copied
 *                        header, so a copy that drops it loops forever and one
 *                        that runs it twice skips elements.
 *   post_inc_cnt      -- `i++ < n`: the loop variable is live AFTER the loop,
 *                        so the exit value must come from the tail copy.
 *   continue_loop     -- several back-edges to one header: only the last is
 *                        rewritten, the `continue` jumps still go to the head.
 *   break_loop        -- an early exit beside the tail exit.
 *   hdr_temp_in_body  -- a header value reused in the body: the copy may not
 *                        rename a temp the body reads.
 *   nested            -- inner loop plus outer loop, both candidates.
 *   alias_bound       -- the bound is re-read from memory and the body writes
 *                        it through an aliasing pointer: the tail copy must
 *                        load it again, not reuse the header's value.
 *   call_body         -- a call in the body that changes what the header reads.
 *   idx_bound         -- the bound is an indexed load (LOAD_INDEXED, whose
 *                        scale sits in a 4th operand slot the copy must carry).
 *   shared_tmp        -- `p[i - 1]` after `i++ < n`: the header's old-value
 *                        temp is read by the body (left un-rotated).
 *   zero/one/many trip counts, unsigned and signed bounds at the extremes.
 */
#include <stdio.h>

static unsigned char src[40], dst[40];

static void *mv_fwd(unsigned char *d, const unsigned char *s, unsigned n)
{
  unsigned char *p = d;
  while (n-- != 0)
    *p++ = *s++;
  return d;
}

static void *mv_bwd(unsigned char *d, const unsigned char *s, unsigned n)
{
  while (n-- != 0)
    d[n] = s[n];
  return d;
}

static unsigned post_inc_cnt(unsigned n)
{
  unsigned i = 0, s = 0;
  while (i++ < n)
    s += i * 3;
  return s * 1000u + i;
}

static int continue_loop(const int *v, int n)
{
  int i = 0, s = 0;
  while (i++ < n)
  {
    if (v[i - 1] & 1)
      continue;
    if (v[i - 1] == 100)
      continue;
    s += v[i - 1];
  }
  return s * 100 + i;
}

static int break_loop(const int *v, int n, int key)
{
  int i = 0;
  while (n-- > 0)
  {
    if (v[i] == key)
      break;
    i++;
  }
  return i * 1000 + n;
}

static int hdr_temp_in_body(const unsigned char *p, unsigned n)
{
  unsigned i = 0;
  int s = 0;
  unsigned c;
  while ((c = i++) < n)
    s += p[c] * (int)(c + 1);
  return s + (int)c;
}

static int nested(int n, int m)
{
  int s = 0, i = 0;
  while (i++ < n)
  {
    int j = 0;
    while (j++ < m)
      s += i * j;
  }
  return s * 10 + i;
}

static unsigned sat_unsigned(unsigned n)
{
  unsigned c = 0;
  while (n-- != 0)
  {
    c++;
    if (c >= 5)
      break;
  }
  return c * 100000u + (n & 0xffff);
}

static int signed_bound(int lo, int hi)
{
  int c = 0;
  int i = lo;
  while (i++ < hi)
    c++;
  return c * 16 + (i == hi + 1);
}

struct slice
{
  int *p;
  unsigned len;
};

static unsigned alias_bound(struct slice *s, int *alias_len)
{
  unsigned i, sum = 0;
  for (i = 0; i != s->len; i++)
  {
    sum = sum * 3u + (unsigned)s->p[i];
    if (s->p[i] == 7)
      *alias_len = (int)i + 2; /* shrinks the bound mid-loop */
  }
  return sum * 100u + i;
}

static int calls;
static unsigned limit = 5;
static int bump(int v)
{
  calls++;
  if (v == 3)
    limit = 2;
  return v + 1;
}

static int call_body(int start)
{
  int v = start;
  unsigned i;
  for (i = 0; i < limit; i++)
    v = bump(v);
  return v * 100 + (int)i;
}

static int idx_bound(const unsigned *b, int k)
{
  int s = 0;
  for (unsigned i = 0; i < b[k]; i++)
    s += (int)(i ^ b[k + 1]);
  return s;
}

static int shared_tmp(const int *p, int n)
{
  int i = 0, s = 0;
  while (i++ < n)
  {
    if (p[i - 1] & 1)
      s += 3;
    else
      s -= p[i - 1];
  }
  return s * 10 + i;
}

int main(void)
{
  static const unsigned counts[] = {0, 1, 2, 3, 7, 8, 9, 31, 40};
  static const unsigned bounds[] = {0, 1, 2, 5, 9, 33};
  static const int vals[] = {3, 8, 100, 6, 5, 100, 12, 7, 20, 1, 4, 9};
  unsigned h = 0;

  for (unsigned i = 0; i < 40; i++)
    src[i] = (unsigned char)(i * 7 + 1);

  for (unsigned k = 0; k < sizeof(counts) / sizeof(counts[0]); k++)
  {
    for (unsigned i = 0; i < 40; i++)
      dst[i] = 0xee;
    mv_fwd(dst, src, counts[k]);
    for (unsigned i = 0; i < 40; i++)
      h = h * 31 + dst[i];
    for (unsigned i = 0; i < 40; i++)
      dst[i] = 0xdd;
    mv_bwd(dst, src, counts[k]);
    for (unsigned i = 0; i < 40; i++)
      h = h * 31 + dst[i];
  }
  printf("copy=%u\n", h);

  for (unsigned k = 0; k < sizeof(bounds) / sizeof(bounds[0]); k++)
    printf("post_inc(%u)=%u\n", bounds[k], post_inc_cnt(bounds[k]));

  for (int n = 0; n <= 12; n += 3)
    printf("continue(%d)=%d\n", n, continue_loop(vals, n));

  for (int n = 0; n <= 12; n += 4)
    printf("break(%d,20)=%d break(%d,99)=%d\n", n, break_loop(vals, n, 20), n, break_loop(vals, n, 99));

  for (unsigned k = 0; k < sizeof(bounds) / sizeof(bounds[0]); k++)
    printf("hdr_temp(%u)=%d\n", bounds[k], hdr_temp_in_body(src, bounds[k]));

  printf("nested=%d %d %d %d\n", nested(0, 3), nested(3, 0), nested(1, 1), nested(4, 5));
  printf("sat=%u %u %u %u\n", sat_unsigned(0), sat_unsigned(3), sat_unsigned(5), sat_unsigned(1000));
  printf("signed=%d %d %d %d\n", signed_bound(-3, 3), signed_bound(5, 5), signed_bound(7, 5),
         signed_bound(2147483640, 2147483646));
  {
    int buf[12] = {1, 2, 3, 4, 7, 6, 5, 9, 10, 11, 12, 13};
    struct slice sl = {buf, 12};
    printf("alias=%u", alias_bound(&sl, (int *)&sl.len));
    sl.len = 12;
    buf[4] = 0;
    printf(" %u", alias_bound(&sl, (int *)&sl.len));
    sl.len = 0;
    printf(" %u\n", alias_bound(&sl, (int *)&sl.len));
  }
  printf("call=%d", call_body(0));
  limit = 5;
  printf(" %d", call_body(10));
  limit = 0;
  printf(" %d calls=%d\n", call_body(1), calls);
  {
    static const unsigned b[] = {0, 3, 1, 9, 4, 0xffffffffu, 2};
    printf("idx=%d %d %d %d\n", idx_bound(b, 0), idx_bound(b, 1), idx_bound(b, 2), idx_bound(b, 4) & 0xff);
  }
  {
    static const int p[] = {4, 5, 8, -3, 2, 7, 6};
    printf("shared=%d %d %d %d\n", shared_tmp(p, 0), shared_tmp(p, 1), shared_tmp(p, 4), shared_tmp(p, 7));
  }
  return 0;
}
