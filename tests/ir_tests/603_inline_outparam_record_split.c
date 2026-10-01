/* awk's record reader (toybox getr/rx_find_rs), reduced: a static helper
 * writes the separator's offsets through pointers to the caller's locals and
 * returns 0; at -O1 and above the caller saw "not found" and returned the
 * whole buffer as one record. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

typedef long regoff_t;
typedef struct { int dummy; } fake_rx;
typedef struct { regoff_t rm_so, rm_eo; } fake_match;
#define REG_NOMATCH 1

static int fake_regexec(fake_rx *rx, const char *s, fake_match *m)
{
  (void)rx;
  const char *p = strchr(s, '\n');
  if (!p) return REG_NOMATCH;
  m->rm_so = p - s;
  m->rm_eo = m->rm_so + 1;
  return 0;
}

struct zfile { const char *src; long srclen, srcpos; char *buf; long buflen, ro, lim; int eof, is_tty; };
static char *recptr;

static long zread(struct zfile *z, char *dst, long m)
{
  long n = z->srclen - z->srcpos;
  if (n > m) n = m;
  memcpy(dst, z->src + z->srcpos, n);
  z->srcpos += n;
  return n;
}

static int rx_find_rs(fake_rx *rx, char *s, long len, regoff_t *start, regoff_t *end, int one_byte_rs)
{
  fake_match matches[1];
  if (one_byte_rs) {
    char *p = memchr(s, one_byte_rs, len);
    if (!p) return REG_NOMATCH;
    *start = p - s;
    *end = *start + 1;
  } else {
    int r = fake_regexec(rx, s, matches);
    if (r == REG_NOMATCH) return r;
    if (r) exit(9);
    *start = matches[0].rm_so;
    *end = matches[0].rm_eo;
  }
  return 0;
}

static long getr(struct zfile *zfp, int rs_mode, const char *rs)
{
  fake_rx rsrx;
  long ret = -1;
  int r = -REG_NOMATCH;
  regoff_t so = 0, eo = 0;
  long m = 0, n = 0;

  rs_mode = strlen(rs) == 1 ? rs[0] : 0;
  for (;;) {
    if (zfp->ro == zfp->lim && zfp->eof) break;
    if (zfp->ro == 0 && zfp->lim == zfp->buflen)
      zfp->buf = realloc(zfp->buf, (zfp->buflen = zfp->buflen * 2 > 512 ? zfp->buflen * 2 : 512) + 1);
    if ((m = zfp->buflen - zfp->lim) && !zfp->eof) {
      if (zfp->is_tty) m = 1;
      n = zread(zfp, zfp->buf + zfp->lim, m);
      if (n < m) {
        zfp->eof = 1;
        if (!n && r == -REG_NOMATCH) break;
      }
      zfp->lim += n;
      zfp->buf[zfp->lim] = 0;
    }
    recptr = zfp->buf + zfp->ro;
    r = rx_find_rs(&rsrx, recptr, zfp->lim - zfp->ro, &so, &eo, rs_mode);
    if (!r && so == eo) r = 1;
    if (!zfp->eof && (r || (zfp->lim - (zfp->ro + eo)) < zfp->buflen / 4) && !zfp->is_tty) {
      memmove(zfp->buf, recptr, zfp->lim - zfp->ro);
      zfp->lim -= zfp->ro;
      zfp->ro = 0;
      continue;
    }
    ret = so;
    if (zfp->eof) {
      if (r) {
        ret = zfp->lim - zfp->ro;
        zfp->ro = zfp->lim;
      } else zfp->ro += eo;
    } else zfp->ro += eo;
    if (!r || !zfp->is_tty) {
      if (zfp->is_tty) zfp->ro = zfp->lim = 0;
      break;
    }
  }
  return ret;
}

int main(void)
{
  static const char text[] = "1 a\n2 b\n3 c\n";
  struct zfile z = {0};
  long k;
  int nr = 0;
  z.src = text;
  z.srclen = sizeof(text) - 1;
  while ((k = getr(&z, 0, "\n")) >= 0)
    printf("rec %d len %ld [%.*s]\n", ++nr, k, (int)k, recptr);
  printf("NR %d\n", nr);
  return 0;
}
