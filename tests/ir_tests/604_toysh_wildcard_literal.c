// toysh's wildcard_matchlen (toybox toys/pending/sh.c), matching a case
// pattern against a word. At -O1 and above the device compiler returned
// failure for any literal character ("e" against "e", "a*" against "ab").
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

struct sh_arg {
  char **v;
  int c;
};

#define WILD_SHORT 1
#define WILD_CASE  2
#define WILD_ANY   4

static void arg_add(struct sh_arg *arg, char *data)
{
  if (!(arg->c&31)) arg->v = realloc(arg->v, sizeof(char *)*(arg->c+33));
  arg->v[arg->c++] = data;
  arg->v[arg->c] = 0;
}

__attribute__((noinline)) static int utf8towc(unsigned *wc, char *str, unsigned len)
{
  if (!len || !*str) return 0;
  *wc = (unsigned char)*str;
  return 1;
}

static int getutf8(char *s, int len, int *cc)
{
  unsigned wc;

  if (len<0) wc = len = 0;
  else if (1>(len = utf8towc(&wc, s, len))) wc = *s, len = 1;
  if (cc) *cc = wc;

  return len;
}

static int wildcard_matchlen(char *str, int len, char *pattern, int plen,
  struct sh_arg *deck, int flags)
{
  struct sh_arg ant = {0};    // stack: of str offsets
  long ss, pp, dd, best = -1;
  int i, j, k, c, not, hit;

  // Loop through wildcards in pattern.
  for (ss = pp = dd = 0; ;) {
    if ((flags&WILD_ANY) && best!=-1) break;

    // did we consume pattern?
    if (pp==plen) {
      if (ss>best) best = ss;
      if (ss==len || (flags&WILD_SHORT)) break;
    // attempt literal match?
    } else if (dd>=deck->c || pp!=(long)deck->v[dd]) {
      if (ss<len) {
        if (flags&WILD_CASE) {
          ss += getutf8(str+ss, len-ss, &c);
          c = towupper(c);
          pp += getutf8(pattern+pp, pp-plen, &i);
          i = towupper(i);
        } else c = str[ss++], i = pattern[pp++];
        if (c==i) continue;
      }

    // Wildcard chars: |+@!*?()[]
    } else {
      c = pattern[pp++];
      dd++;
      if (c=='?' || ((flags&WILD_ANY) && c=='*')) {
        ss += (i = getutf8(str+ss, len-ss, 0));
        if (i) continue;
      } else if (c=='*') {

        // start with zero length match, don't record consecutive **
        if (dd==1 || pp-2!=(long)deck->v[dd-1] || pattern[pp-2]!='*') {
          arg_add(&ant, (void *)ss);
          arg_add(&ant, 0);
        }

        continue;
      } else if (c == '[') {
        // next char of str in the set (or not in it, after [! or [^)
        pp += (not = !!strchr("!^", pattern[pp]));
        ss += (k = getutf8(str+ss, len-ss, &c));
        if (flags&WILD_CASE) c = towupper(c);
        for (hit = 0; pp<(long)deck->v[dd];) {
          pp += getutf8(pattern+pp, plen-pp, &i);
          j = i;
          if (pattern[pp]=='-' && pp+1<(long)deck->v[dd])
            pp += 1+getutf8(pattern+pp+1, plen-pp-1, &j);
          if (flags&WILD_CASE) i = towupper(i), j = towupper(j);
          hit |= i<=c && c<=j;
        }
        if (k && hit!=not) {
          pp = 1+(long)deck->v[dd++];

          continue;
        }

      // ( preceded by +@!*?

      } else { // TODO ( ) |
        dd++;
        continue;
      }
    }

    // match failure
    if (flags&WILD_ANY) {
      ss = 0;
      if (plen==pp) break;
      continue;
    }

    // pop retry stack or return failure (TODO: seek to next | in paren)
    while (ant.c) {
      if ((c = pattern[(long)deck->v[--dd]])=='*') {
        if (len<(ss = (long)ant.v[ant.c-2]+(long)++ant.v[ant.c-1])) ant.c -= 2;
        else {
          pp = (long)deck->v[dd++]+1;
          break;
        }
      } else if (c == '(') dprintf(2, "TODO: (");
    }

    if (!ant.c) break;
  }
  free (ant.v);

  return best;
}

static int wildcard_match(char *s, char *p, struct sh_arg *deck, int flags)
{
  return wildcard_matchlen(s, strlen(s), p, strlen(p), deck, flags);
}

static void check(char *s, char *p, long *wild, int nwild)
{
  struct sh_arg deck = {(char **)wild, nwild};

  printf("%s ~ %s: %d\n", s, p, wildcard_match(s, p, &deck, 0));
}

int main(void)
{
  long star[] = {1}, q[] = {0}, br[] = {0, 2}, st2[] = {0, 3};

  check("e", "e", 0, 0);
  check("ab", "a*", star, 1);
  check("e", "?", q, 1);
  check("e", "[e]", br, 2);
  check("b", "[!a]", st2, 2);
  check("a", "[!a]", st2, 2);
  check("abc", "abd", 0, 0);

  return 0;
}
