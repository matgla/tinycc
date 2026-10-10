int printf(const char *, ...);

/* ssa:reroll inserts its counter init in front of the run; a branch to the
 * run's first instruction (the init loop's exit) was shifted past it and the
 * counter started undefined: three calls of four. */
#define NI __attribute__((noinline))
#define N 5
typedef struct { unsigned a[N]; } B;
static NI void chk(const B *b) { printf("chk"); for (int i = 0; i < N; i++) printf(" %u", b->a[i]); printf("\n"); }

int main(void)
{
  B L0;
  for (int i = 0; i < N; i++) L0.a[i] = 45u + i;
  chk(&L0);
  chk(&L0);
  chk(&L0);
  chk(&L0);
  return 0;
}
