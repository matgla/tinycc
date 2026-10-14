// The yasos kernel's HardFault handler through Zig's C backend and tcc -O2:
// `mrs %[out], psp` into a local that register pressure had spilled.  Inline-
// asm lowering resolves each operand once (tcc_ir_fill_registers: c.i becomes
// the vreg's final frame home), then turned that SValue back into an IR
// operand that still carried the vreg -- and machine_op_from_ir resolved it a
// second time, reading c.i as the front end's offset and adding
// `c.i - original_offset` again.  The output landed below SP ([sp, #-488]),
// the handler built the dying process's exit frame from a stale PSP, and a
// parent whose vfork child crashed faulted forever on resume.  Inputs of the
// same shape loaded from the wrong slot too, and with callee-saved clobbers
// the loads and stores inside the asm's own register save missed its bytes.
// scribble() leaves 0xAA.. below SP so a stray slot shows.
#include <stdio.h>

volatile unsigned src[24];
unsigned sink[24];

__attribute__((noinline)) void scribble(void)
{
  volatile unsigned buf[128];
  for (int i = 0; i < 128; i++)
    buf[i] = 0xAAAAAAAAu;
}

__attribute__((noinline)) void use(unsigned *p) { (void)p; }

/* Outputs: each asm result is copied into an older variable that is then
 * redefined, the way the C backend reuses its temporaries, with enough
 * live values that the results go to the stack. */
__attribute__((noinline)) unsigned outputs(void)
{
  unsigned v0 = src[0], v1 = src[1], v2 = src[2], v3 = src[3], v4 = src[4], v5 = src[5];
  unsigned v6 = src[6], v7 = src[7], v8 = src[8], v9 = src[9], v10 = src[10], v11 = src[11];
  unsigned t15, t16, t17, t18, t19;
  unsigned buf[4];
  use(buf);
  __asm volatile("movw %[out], #0x111" : [out] "=r"(t16));
  t15 = t16;
  __asm volatile("movw %[out], #0x222" : [out] "=r"(t17));
  t16 = t17;
  __asm volatile("movw %[out], #0x333" : [out] "=r"(t18));
  t17 = t18;
  __asm volatile("movw %[out], #0x444" : [out] "=r"(t19));
  t18 = t19;
  use(buf);
  sink[0] = t15;
  sink[1] = t16;
  sink[2] = t17;
  sink[3] = t18;
  return v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 + v11 + t15 + t16 + t17 + t18;
}

/* Inputs, and a "+r", whose variables live on the stack across a call. */
__attribute__((noinline)) unsigned inputs(void)
{
  unsigned v0 = src[0], v1 = src[1], v2 = src[2], v3 = src[3], v4 = src[4], v5 = src[5];
  unsigned v6 = src[6], v7 = src[7], v8 = src[8], v9 = src[9], v10 = src[10], v11 = src[11];
  unsigned a = src[12] * 3, b = src[13] * 5, acc = src[14];
  unsigned buf[4];
  use(buf);
  __asm volatile("add %0, %0, %1\n\tadd %0, %0, %2" : "+r"(acc) : "r"(a), "r"(b));
  use(buf);
  sink[4] = acc;
  return v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 + v11 + a + b + acc;
}

/* The same with the asm clobbering callee-saved registers, so the operand
 * loads and stores run inside the asm's own register save. */
__attribute__((noinline)) unsigned clobbering(void)
{
  unsigned v0 = src[0], v1 = src[1], v2 = src[2], v3 = src[3], v4 = src[4], v5 = src[5];
  unsigned v6 = src[6], v7 = src[7], v8 = src[8], v9 = src[9], v10 = src[10], v11 = src[11];
  unsigned a = src[15] * 7, out;
  unsigned buf[4];
  use(buf);
  __asm volatile("mov r4, #1\n\tmov r5, #2\n\tadd %0, %1, r4\n\tadd %0, %0, r5" : "=r"(out) : "r"(a) : "r4", "r5", "r6", "r7", "r8");
  use(buf);
  sink[5] = out;
  return v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 + v11 + a + out;
}

int main(void)
{
  for (int i = 0; i < 24; i++)
    src[i] = i + 1;
  scribble();
  unsigned r0 = outputs();
  scribble();
  unsigned r1 = inputs();
  scribble();
  unsigned r2 = clobbering();
  printf("out %x %x %x %x sum=%u\n", sink[0], sink[1], sink[2], sink[3], r0);
  printf("in acc=%u sum=%u\n", sink[4], r1);
  printf("clob out=%u sum=%u\n", sink[5], r2);
  return 0;
}
