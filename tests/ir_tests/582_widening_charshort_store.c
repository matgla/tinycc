/* A store of a narrower integer into a char/short object: `unsigned short v =
   uchar;`.  vstore delays char/short casts, and the delayed cast treats the
   value as an int -- right for a narrowing store (int -> short wraps at the
   store), wrong for a widening one, whose unsigned byte was read back
   sign-extended (200 -> 65480).  Widening stores now cast at once. */
#include <stdio.h>

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef short s16;
typedef _Bool b1;

static u8 ubytes[4] = {0, 200, 255, 127};
static s8 sbytes[4] = {0, -56, -1, 127};
static u16 uhalf[2] = {0xFFFF, 0x8000};

__attribute__((noinline)) unsigned from_param(u8 x) { u16 v = x; return v; }
__attribute__((noinline)) int from_param_s(u8 x) { s16 v = x; return v; }
__attribute__((noinline)) unsigned from_array(int i) { u16 v; v = ubytes[i]; return v; }
__attribute__((noinline)) unsigned from_ptr(const u8 *p) { u16 v = *p; return v; }
__attribute__((noinline)) unsigned from_signed(int i) { u16 v = sbytes[i]; return v; }
__attribute__((noinline)) int from_signed_s(int i) { s16 v = sbytes[i]; return v; }
__attribute__((noinline)) unsigned from_bool(b1 b) { u16 v = b; return v; }
__attribute__((noinline)) unsigned narrow_int(int x) { u8 v = x; return v; }
__attribute__((noinline)) unsigned narrow_half(int i) { u8 v = uhalf[i]; s8 w = uhalf[i]; return v * 1000u + (unsigned)(w + 200); }

int main(void)
{
  for (int i = 0; i < 4; i++)
    printf("%d: %u %d %u %u %u %d\n", i, from_param(ubytes[i]), from_param_s(ubytes[i]), from_array(i),
           from_ptr(&ubytes[i]), from_signed(i), from_signed_s(i));
  printf("bool %u %u\n", from_bool(0), from_bool(1));
  printf("narrow %u %u %u %u\n", narrow_int(0x1234), narrow_int(-1), narrow_half(0), narrow_half(1));
  return 0;
}
