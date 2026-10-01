/* Branch narrowing and CBZ fusion trust the rehearsal pass's layout.
 *
 * The rehearsal is a dry run: it laid out a mid-function return's branch to
 * the epilogue as zero bytes (only the real pass emitted it) and an inline asm
 * body as zero bytes (dry runs never assemble it).  A range holding either was
 * longer than modelled, and a fused CBZ, which cannot be widened once
 * committed, failed with "CBZ/CBNZ target out of range: offset=128" (a return,
 * found compiling the Zig compiler's C) or offset=148 (an asm body). */
#include <stdio.h>

volatile int g;

/* A 2-byte body step walks the fused CBZ across its 126-byte limit; one of
 * these lands where the return's branch pushed the target to 128. */
#define ADD1 y += x;
#define ADD2 ADD1 ADD1
#define ADD4 ADD2 ADD2
#define ADD8 ADD4 ADD4
#define ADD16 ADD8 ADD8
#define ADD32 ADD16 ADD16
#define EARLY_RETURN(k, body)                                                                                         \
  int adds_##k(int x, int y)                                                                                          \
  {                                                                                                                   \
    if (x)                                                                                                            \
    {                                                                                                                 \
      body return y;                                                                                                  \
    }                                                                                                                 \
    return g;                                                                                                         \
  }

EARLY_RETURN(40, ADD32 ADD8)
EARLY_RETURN(41, ADD32 ADD8 ADD1)
EARLY_RETURN(42, ADD32 ADD8 ADD2)
EARLY_RETURN(43, ADD32 ADD8 ADD2 ADD1)
EARLY_RETURN(44, ADD32 ADD8 ADD4)
EARLY_RETURN(45, ADD32 ADD8 ADD4 ADD1)
EARLY_RETURN(46, ADD32 ADD8 ADD4 ADD2)
EARLY_RETURN(47, ADD32 ADD8 ADD4 ADD2 ADD1)
EARLY_RETURN(48, ADD32 ADD16)
EARLY_RETURN(49, ADD32 ADD16 ADD1)
EARLY_RETURN(50, ADD32 ADD16 ADD2)
EARLY_RETURN(51, ADD32 ADD16 ADD2 ADD1)
EARLY_RETURN(52, ADD32 ADD16 ADD4)
EARLY_RETURN(53, ADD32 ADD16 ADD4 ADD1)
EARLY_RETURN(54, ADD32 ADD16 ADD4 ADD2)
EARLY_RETURN(55, ADD32 ADD16 ADD4 ADD2 ADD1)
EARLY_RETURN(56, ADD32 ADD16 ADD8)
EARLY_RETURN(57, ADD32 ADD16 ADD8 ADD1)
EARLY_RETURN(58, ADD32 ADD16 ADD8 ADD2)
EARLY_RETURN(59, ADD32 ADD16 ADD8 ADD2 ADD1)
EARLY_RETURN(60, ADD32 ADD16 ADD8 ADD4)
EARLY_RETURN(61, ADD32 ADD16 ADD8 ADD4 ADD1)
EARLY_RETURN(62, ADD32 ADD16 ADD8 ADD4 ADD2)
EARLY_RETURN(63, ADD32 ADD16 ADD8 ADD4 ADD2 ADD1)
EARLY_RETURN(64, ADD32 ADD32)
EARLY_RETURN(65, ADD32 ADD32 ADD1)
EARLY_RETURN(66, ADD32 ADD32 ADD2)
EARLY_RETURN(67, ADD32 ADD32 ADD2 ADD1)
EARLY_RETURN(68, ADD32 ADD32 ADD4)
EARLY_RETURN(69, ADD32 ADD32 ADD4 ADD1)
EARLY_RETURN(70, ADD32 ADD32 ADD4 ADD2)
EARLY_RETURN(71, ADD32 ADD32 ADD4 ADD2 ADD1)
EARLY_RETURN(72, ADD32 ADD32 ADD8)
EARLY_RETURN(73, ADD32 ADD32 ADD8 ADD1)
EARLY_RETURN(74, ADD32 ADD32 ADD8 ADD2)
EARLY_RETURN(75, ADD32 ADD32 ADD8 ADD2 ADD1)
EARLY_RETURN(76, ADD32 ADD32 ADD8 ADD4)
EARLY_RETURN(77, ADD32 ADD32 ADD8 ADD4 ADD1)
EARLY_RETURN(78, ADD32 ADD32 ADD8 ADD4 ADD2)
EARLY_RETURN(79, ADD32 ADD32 ADD8 ADD4 ADD2 ADD1)

static int (*const early_returns[])(int, int) = {
    adds_40, adds_41, adds_42, adds_43, adds_44, adds_45, adds_46, adds_47,
    adds_48, adds_49, adds_50, adds_51, adds_52, adds_53, adds_54, adds_55,
    adds_56, adds_57, adds_58, adds_59, adds_60, adds_61, adds_62, adds_63,
    adds_64, adds_65, adds_66, adds_67, adds_68, adds_69, adds_70, adds_71,
    adds_72, adds_73, adds_74, adds_75, adds_76, adds_77, adds_78, adds_79,
};

/* 144 bytes of asm body the rehearsal saw as nothing. */
#define NOP8 "nop\n nop\n nop\n nop\n nop\n nop\n nop\n nop\n"
int over_asm(int x)
{
  if (x)
  {
    __asm volatile(NOP8 NOP8 NOP8 NOP8 NOP8 NOP8 NOP8 NOP8 NOP8);
    g = 1;
  }
  return g;
}

int main(void)
{
  int sum = 0;
  for (unsigned i = 0; i < sizeof early_returns / sizeof early_returns[0]; i++)
    sum += early_returns[i](1, 0) + early_returns[i](0, 0);
  printf("%d\n", sum); /* 40 + 41 + ... + 79 */
  printf("%d\n", over_asm(0));
  printf("%d\n", over_asm(1));
  return 0;
}
