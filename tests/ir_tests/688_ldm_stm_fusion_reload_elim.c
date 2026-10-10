/* A frame-slot word copy fused into LDM/STM (codegen's load+store run) took
   its loaded temps' register for free because each temp's one use was its
   STORE -- but ra:reload_elim had dropped the later reload of the last copied
   word (`t1.a[2]`) because that register still held it.  `stmia r0!` left r0
   an address, and the elided reload passed it to mk_ptr as data. */
#include <stdio.h>
#define NI __attribute__((noinline))
#define N 3
typedef struct { unsigned a[N]; } B;
static B G;
static const B CG = {{ 920u, 996u, 851u }};
static B *stash;
static unsigned acc;
static NI void chk(const B *b) { printf("chk"); for (int i = 0; i < N; i++) printf(" %u", b->a[i]); printf("\n"); }
static NI B mk(unsigned k) { B r; for (int i = 0; i < N; i++) r.a[i] = k * 3u + i; return r; }
static NI B mk_ptr(const B *s, unsigned k) { B r; for (int i = 0; i < N; i++) r.a[i] = s->a[i] ^ k; return r; }
static NI unsigned rd(void) { unsigned s = 0; for (int i = 0; i < N; i++) s = s * 7u + stash->a[i]; return s; }
static B f1(unsigned k, B *pp) {
  B r = *pp;
  B t0 = *pp;
  B t1 = CG;
  switch (k % 6u) { case 2: stash = &r; acc += rd(); break; default: acc += t1.a[0]; }
  if (k & 2) { return t1; }
  return r;
}
static B f0(unsigned k, B *pp) {
  B r = mk(k);
  B t0 = *pp;
  B t1 = f1(k + 511u, &r);
  switch (k % 6u) { case 2: stash = &r; acc += rd(); break; case 3: t0 = r; break; default: acc += t1.a[0]; }
  t1 = mk_ptr(&r, t1.a[2]);
  return t1;
}
int main(void) {
  B L0 = f0(513u, &G);
  B P; for (unsigned i = 0; i < acc % 7u + 3u; i++) P.a[i % N] = 145u * i; chk(&P);
  chk(&L0);
}