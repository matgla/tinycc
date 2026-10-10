/* An inlined body's locals die where the body ends (tcc_ir_frame_scope_end):
   frame relayout may give an escaped one's bytes to another object after
   that point.  The end is an instruction index, so it has to move with the
   code.  Here main's init loops are unrolled in front of the inlined f0, which
   pushes its body ~100 instructions down; with the end left behind, `r`
   (escaped to mk's result, chk and mk_ptr) shared its slot with t0, and
   `t0 = CG` overwrote it before chk(&r) read it.  The shape is fragile:
   keep it as is (685 has the variations). */
int printf(const char *, ...);
#define NI __attribute__((noinline))
#define N 6
typedef struct { unsigned a[N]; } B;
static B G;
static const B CG = {{ 515u, 754u, 937u, 209u, 652u, 789u }};
static B *stash;
static NI void chk(const B *b) { printf("chk %u %u %u %u %u %u\n", b->a[0],b->a[1],b->a[2],b->a[3],b->a[4],b->a[5]); }
static NI B mk(unsigned k) { B r; for (int i = 0; i < N; i++) r.a[i] = k * 3u + i; return r; }
static NI B mk_ptr(const B *s, unsigned k) { B r; for (int i = 0; i < N; i++) r.a[i] = s->a[i] ^ k; return r; }
static B f0(unsigned k, B *pp) {
  B *saved = stash;
  B r = mk(k);
  B t0 = CG;
  B t1 = G;
  if (t1.a[3] & 1) { t0.a[4] ^= k; } else {
  chk(&r);
  { B t = CG; t.a[5] = r.a[1]; r = t; }
  }
  r.a[1] ^= t0.a[5];
  G = r;
  t1 = mk_ptr(&r, t1.a[2]);
  if (k & 4) { stash = saved; return t0; }
  stash = saved; return r;
}
int main(void) {
  B L0; for (int i = 0; i < N; i++) L0.a[i] = 76u + i;
  B L1; for (int i = 0; i < N; i++) L1.a[i] = 85u + i;
  B L2; for (int i = 0; i < N; i++) L2.a[i] = 24u + i;
  B L3; for (int i = 0; i < N; i++) L3.a[i] = 36u + i;
  L1 = f0(30u, &G);
  chk(&L0);
  return 0;
}
