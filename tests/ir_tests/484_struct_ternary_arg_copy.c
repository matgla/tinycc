/* `f(c ? g() : h())` where g and h return an 8-byte struct through sret: the
   selected result is copied into the argument slot a word at a time, and
   dead_temp_local took the by-value struct argument that reads the slot as a
   4-byte read, so it dropped the store of the second word.  f got the first
   word and whatever the stack held after it.  The self-hosted tcc builds its
   Thumb opcodes this way (`ot_check(u ? th_uxtb(..) : th_sxtb(..))`, an
   8-byte thumb_opcode) and emitted 0x0000 for them.

   scribble() fills the stack first, so an uncopied word is 0xA5A5A5A5 on any
   target rather than a lucky zero. */
#include <stdint.h>
#include <stdio.h>

typedef struct op
{
  uint8_t size;
  uint32_t opcode;
} op;

typedef struct mo
{
  int kind;
  unsigned is_unsigned : 1;
  unsigned needs_deref : 1;
  int btype;
} mo;

typedef struct big
{
  int a, b, c, d, e;
} big;

__attribute__((noinline)) void scribble(void)
{
  volatile unsigned char junk[256];
  for (int i = 0; i < 256; i++)
    junk[i] = 0xA5;
}

__attribute__((noinline)) op mk_u(int r)
{
  op o = {2, 0xb2c0u | (unsigned)r};
  return o;
}

__attribute__((noinline)) op mk_s(int r)
{
  op o = {2, 0xb240u | (unsigned)r};
  return o;
}

__attribute__((noinline)) big mk_big(int k)
{
  big b = {k, k + 1, k + 2, k + 3, k + 4};
  return b;
}

__attribute__((noinline)) void check(op o) { printf("%u %04x\n", o.size, (unsigned)o.opcode); }

__attribute__((noinline)) void check_big(big b) { printf("%d %d %d %d %d\n", b.a, b.b, b.c, b.d, b.e); }

__attribute__((noinline)) void via_bitfield(mo d, int r) { check(d.is_unsigned ? mk_u(r) : mk_s(r)); }

__attribute__((noinline)) void via_int(int u, int r) { check(u ? mk_u(r) : mk_s(r)); }

__attribute__((noinline)) void via_big(int u) { check_big(u ? mk_big(10) : mk_big(20)); }

int main(void)
{
  mo d = {3, 1, 0, 1};
  scribble();
  via_bitfield(d, 1);
  d.is_unsigned = 0;
  scribble();
  via_bitfield(d, 2);
  scribble();
  via_int(1, 3);
  scribble();
  via_int(0, 4);
  scribble();
  via_big(1);
  scribble();
  via_big(0);
  return 0;
}
