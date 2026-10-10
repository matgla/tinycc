/* codegen ADD/SUB + CMP #0 flag fusion skips a CMP that is a branch target.
 *
 * `tcc_ir_codegen_generate` (source/ir/codegen.c, case TCCIR_OP_ADD/SUB) fuses
 * `Vd <- a SUB b` with an immediately following `CMP Vd,#0` that feeds an EQ/NE
 * JUMPIF: it emits a flag-setting `subs` and sets codegen_skip_cmp so the CMP
 * emits nothing.  The peephole indexes compact_instructions[i+1] directly and
 * never asks whether that CMP is a jump target, nor whether its operand is a
 * volatile access.
 *
 * A volatile (or address-taken) count-down latch keeps the loop in top-test
 * form: the back edge jumps to the CMP's address, which is now the `beq`
 * itself, so the branch reads whatever flags the loop body left behind, and the
 * volatile slot is never re-read for the test.  Wrong at every -O level
 * (vol=1 at -O0, vol=0 at -O1/-O2/-Os); correct is 10.  The plain register
 * loop and the straight-line SUB+CMP are the fusion's legitimate cases and
 * must stay correct. */
int printf(const char *, ...);

__attribute__((noinline)) int count_volatile(int n, const int *g, int *cnt)
{
  volatile int x = n - 1;
  while (x != 0) { x = x - 1; if (*g == 3) *cnt += 1; }
  return *cnt;
}

/* Plain register count-down: the fusion is legitimate, must stay correct. */
__attribute__((noinline)) int count_plain(int n)
{
  int x = n - 1;
  int c = 0;
  while (x != 0) { x = x - 1; c += 1; }
  return c;
}

/* Straight-line SUB + CMP #0 feeding a branch: not a jump target, the fusion
 * must still fire and be correct. */
__attribute__((noinline)) int straight(int n)
{
  int x = n - 1;
  return x != 0 ? 1 : 0;
}

int main(void)
{
  int g = 3, cnt = 0;
  printf("vol: %d\n", count_volatile(11, &g, &cnt));
  printf("plain: %d\n", count_plain(11));
  printf("straight: %d%d\n", straight(11), straight(1));
  return 0;
}
