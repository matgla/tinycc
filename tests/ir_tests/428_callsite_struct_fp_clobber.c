/* Guard: marshaling a by-value struct to the outgoing STACK argument area
 * must not hand the frame pointer out as a data scratch register.
 *
 * find_call_scratch()'s extended path considers prologue-pushed r4-r11 that
 * liveness reports dead -- but r7 (the frame base) is not a liveness
 * interval, so in a frame-pointer function it ALWAYS looks dead.  Unlike its
 * sibling scratch_pushed_dead_reg() it did not reserve R_FP, so the paired
 * word copy of a stack-passed struct picked (lr, r7):
 *
 *     sub.w  ip, r7, #12      ; ip = &op (frame slot)
 *     ldr.w  lr, [ip, #4]
 *     ldr.w  r7, [ip, #8]     ; frame pointer clobbered with a data word
 *     strd   lr, r7, [sp]
 *     sub.w  ip, r7, #12      ; next arg's address computed from garbage
 *     ldr.w  r3, [ip]         ; -> DACCVIOL on device
 *
 * Hit by the -O2 compiler compiling ITSELF (global_addr_hoist.c
 * lac_slot_key -> gah_slot_classify), which crashed every on-device -O2
 * compile.  The same path could hand out r9 (the GOT base under
 * text_and_data_separation).
 *
 * The trigger shape: a frame-pointer function forwarding a 9-byte packed
 * struct (split r3 + 2 stack words) plus enough further args that the
 * marshal needs a second data register while r0-r3/ip/lr are all taken.
 */

#include <stdio.h>

typedef struct __attribute__((packed))
{
  unsigned int a;
  unsigned int b;
  unsigned char c;
} S; /* 9 bytes: word 0 in r3, words 1-2 on the stack */

int sink;

/* External-linkage callee: checks every word survived the marshal. */
int g(void *p, int x, int slot, S op, int **out1, int *out2)
{
  if (p != &sink || x != 0x11223344 || slot != 5)
    return -1;
  if (op.a != 0xCAFEBABEu || op.b != 0x0BADF00Du || op.c != 0x5A)
    return -2;
  *out1 = &sink;
  *out2 = 0x7654;
  return 1;
}

/* The lac_slot_key shape: reassemble the by-value struct into a frame slot,
 * take addresses of locals (forcing the frame pointer), forward everything. */
int f(void *p, int x, int slot, S op, int **out1, int *out2)
{
  int *sym;
  int addend;
  if (g(p, x, slot, op, &sym, &addend) != 1)
    return 0;
  *out1 = sym;
  *out2 = addend;
  return 1;
}

int main(void)
{
  S op;
  int *o1 = 0;
  int o2 = 0;
  op.a = 0xCAFEBABEu;
  op.b = 0x0BADF00Du;
  op.c = 0x5A;
  int r = f(&sink, 0x11223344, 5, op, &o1, &o2);
  printf("r=%d o1=%s o2=%x\n", r, o1 == &sink ? "sink" : "BAD", o2);
  /* Second call proves f's frame survived the first marshal intact. */
  r = f(&sink, 0x11223344, 5, op, &o1, &o2);
  printf("again r=%d o2=%x\n", r, o2);
  return 0;
}
