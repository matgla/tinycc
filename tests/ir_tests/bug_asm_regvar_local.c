/* Local register variables (`register T x __asm("rN")`) feeding inline asm.
 *
 * Zig's C backend emits its Linux syscall wrappers in exactly this shape.  The
 * variable used to get no vreg, so its accesses went to a stack slot that was
 * never allocated and aliased the previous local: at -O0 `tmp = &t0`
 * overwrote t0.array[0], and at -O1 the constraint solver rejected the asm
 * with "asm regvar requests register that's taken already". */
#include <stdio.h>

typedef unsigned int u32;
struct pair
{
  u32 array[2];
};

__attribute__((noinline)) static u32 load_through_pinned(u32 n)
{
  struct pair t0;
  t0.array[0] = n;
  t0.array[1] = 7;
  register u32 ret __asm("r0");
  register struct pair *const tmp __asm("r1") = &t0;
  __asm volatile("ldr %[ret], [%[tmp]]" : [ret] "=r"(ret) : [tmp] "r"(tmp) : "memory");
  return ret + t0.array[0] + t0.array[1];
}

/* The output and the first input name the same register, as in syscall3. */
__attribute__((noinline)) static u32 output_shares_input_reg(u32 a1, u32 a2, u32 a3)
{
  register u32 ret __asm("r0");
  register u32 const x1 __asm("r0") = a1;
  register u32 const x2 __asm("r1") = a2;
  register u32 const x3 __asm("r2") = a3;
  __asm volatile("add %[ret], %[a1], %[a2]\n add %[ret], %[ret], %[a3]"
                 : [ret] "=r"(ret)
                 : [a1] "r"(x1), [a2] "r"(x2), [a3] "r"(x3));
  return ret;
}

/* The asm body names r0/r1 itself, as a syscall does: correct only if the
 * operands really are in the registers the variables were pinned to. */
__attribute__((noinline)) static u32 body_names_registers(u32 b)
{
  register u32 in __asm("r1") = b;
  register u32 out __asm("r0");
  __asm volatile("adds r0, r1, #1" : "=r"(out) : "r"(in) : "cc");
  return out;
}

int main(void)
{
  printf("%u\n", load_through_pinned(42));         /* 42 + 42 + 7 */
  printf("%u\n", output_shares_input_reg(1, 20, 300)); /* 321 */
  printf("%u\n", body_names_registers(41));           /* 42 */
  return 0;
}
