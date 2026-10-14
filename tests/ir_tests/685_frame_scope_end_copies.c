/* Escaped locals of inlined bodies whose code ends up past the recorded end
   (see 684): the body inlined in a constant-trip loop that is unrolled, so
   the copies run past the end the first copy recorded; and an inlined local
   whose address a later call reads back through a global. */
#include <stdio.h>

#define NI __attribute__((noinline))
#define N 6

typedef struct
{
  unsigned a[N];
} B;

static B G;
static const B CG = {{515u, 754u, 937u, 209u, 652u, 789u}};
static B *stash;

static NI void chk(const B *b)
{
  printf("chk %u %u %u %u %u %u\n", b->a[0], b->a[1], b->a[2], b->a[3], b->a[4], b->a[5]);
}

static NI B mk(unsigned k)
{
  B r;
  for (int i = 0; i < N; i++)
    r.a[i] = k * 3u + i;
  return r;
}

static NI B mk_ptr(const B *s, unsigned k)
{
  B r;
  for (int i = 0; i < N; i++)
    r.a[i] = s->a[i] ^ k;
  return r;
}

static B f0(unsigned k, B *pp)
{
  B *saved = stash;
  B r = mk(k);
  B t0 = CG;
  B t1 = G;
  if (t1.a[3] & 1)
  {
    t0.a[4] ^= k;
  }
  else
  {
    chk(&r);
    {
      B t = CG;
      t.a[5] = r.a[1];
      r = t;
    }
  }
  r.a[1] ^= t0.a[5];
  G = r;
  t1 = mk_ptr(&r, t1.a[2]);
  (void)pp;
  if (k & 4)
  {
    stash = saved;
    return t0;
  }
  stash = saved;
  return r;
}

/* f0 inlined in a constant-trip loop: unrolling copies the body past the
   end the first copy recorded */
static NI unsigned in_loop(void)
{
  unsigned s = 0;
  for (unsigned k = 1; k < 4; k++)
  {
    B v = f0(k * 10u, &G);
    s += v.a[0] + v.a[1] + v.a[5];
  }
  return s;
}

/* a pointer to an inlined local read back by a later call through stash */
static NI unsigned via_stash(const B *p)
{
  (void)p;
  return stash->a[2] + stash->a[4];
}

static unsigned g1(unsigned k)
{
  B loc = mk(k);
  B *old = stash;
  stash = &loc;
  B other = CG;
  other.a[0] += k;
  unsigned v = via_stash(&other) + other.a[0];
  stash = old;
  return v;
}

int main(void)
{
  printf("loop %u\n", in_loop());
  unsigned t = 0;
  for (unsigned k = 1; k < 4; k++)
    t += g1(k);
  printf("stash %u\n", t);
  return 0;
}
