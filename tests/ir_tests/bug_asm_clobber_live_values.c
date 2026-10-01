/* Values live across an inline asm statement must survive it.
 *
 * The register allocator did not know what an asm statement destroys (its
 * clobber list, the registers picked for its operands), and asm_gen_code only
 * preserves callee-saved r4-r11.  A value left in r0-r3/r12 across the asm was
 * lost: `clobbered` returned 7 at -O0 and 0 at -O1/-O2. */
#include <stdio.h>

typedef unsigned int u32;

/* A value live across the asm while register variables claim r0 and r1. */
__attribute__((noinline)) static u32 pinned(u32 a, u32 b)
{
  u32 keep = a * 3u + 1u;
  register u32 out __asm("r0");
  register u32 in __asm("r1") = b;
  __asm volatile("add %[o], %[i], #1" : [o] "=r"(out) : [i] "r"(in));
  return keep + out * 100u;
}

/* Values live across an asm that clobbers every caller-saved register. */
__attribute__((noinline)) static u32 clobbered(u32 a, u32 b, u32 c, u32 d)
{
  u32 x = a * 7u, y = b * 11u, z = c * 13u, w = d * 17u;
  __asm volatile("movs r0, #0\n movs r1, #0\n movs r2, #0\n movs r3, #0\n mov r12, r0"
                 :
                 :
                 : "r0", "r1", "r2", "r3", "r12", "cc");
  return x + y + z + w;
}

int main(void)
{
  printf("%u\n", pinned(5, 9));          /* 16 + 10 * 100 */
  printf("%u\n", clobbered(1, 2, 3, 4)); /* 7 + 22 + 39 + 68 */
  return 0;
}
