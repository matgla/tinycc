/* A char or short local whose address is taken lives in its stack home, and
   a store through the pointer writes only its byte or halfword.  Passed as
   a variadic argument (no promotion in the IR) or to a char/short parameter,
   the value went to the argument register with a word load, so the bytes
   above it came from whatever the slot held before: on the device
   ir_tests/478 printed b8=aaaaaaa5 h16=aaaabeef.

   scribble() leaves 0xA5 in the stack the callees below reuse, so a word
   load shows up on any target, not just on one whose stack starts dirty. */
#include <stdio.h>
#include <stdint.h>

__attribute__((noinline)) void scribble(void)
{
  volatile uint8_t junk[128];
  for (int i = 0; i < 128; i++)
    junk[i] = 0xA5;
}

__attribute__((noinline)) void put8(uint8_t *p, uint8_t v) { *p = v; }
__attribute__((noinline)) void put16(uint16_t *p, uint16_t v) { *p = v; }

/* Compares the whole register: a caller that loaded a word hands in bits
   above the byte, which `v == 0x5a` would not look at after its own cast. */
__attribute__((noinline)) int arg_is(uint32_t reg, uint32_t want) { return reg == want; }
__attribute__((noinline)) uint32_t raw_u8(uint8_t v)
{
  uint32_t r;
  __asm__("mov %0, %1" : "=r"(r) : "r"(v));
  return r;
}

__attribute__((noinline)) void variadic(void)
{
  uint8_t u8;
  int8_t s8;
  uint16_t u16;
  int16_t s16;
  put8(&u8, 0x5a);
  put8((uint8_t *)&s8, 0xfb);
  put16(&u16, 0xbeef);
  put16((uint16_t *)&s16, 0xfffe);
  printf("variadic: u8=%x s8=%d u16=%x s16=%d\n", u8, s8, u16, s16);
}

__attribute__((noinline)) void narrow_param(void)
{
  uint8_t u8;
  put8(&u8, 0x5a);
  printf("narrow param: %s\n", arg_is(raw_u8(u8), 0x5a) ? "zero-extended" : "garbage above the byte");
}

int main(void)
{
  scribble();
  variadic();
  scribble();
  narrow_param();
  return 0;
}
