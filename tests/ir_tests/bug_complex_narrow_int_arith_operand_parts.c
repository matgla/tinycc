/* Complex integer + - * / promoted each operand by casting the whole packed
 * pair, so a `_Complex short`/`_Complex char` operand was read as one wide
 * word and its imaginary half came from past the end of the object (and
 * constants were unpacked with the promoted shift).  Read every component at
 * the operand's own element width, then widen it. */
#include <stdio.h>

__attribute__((noinline)) void add_s(_Complex short *pa, _Complex short *pb, _Complex short *pc) { *pc = *pa + *pb; }
__attribute__((noinline)) void sub_s(_Complex short *pa, _Complex short *pb, _Complex short *pc) { *pc = *pa - *pb; }
__attribute__((noinline)) void mul_s(_Complex short *pa, _Complex short *pb, _Complex short *pc) { *pc = *pa * *pb; }
__attribute__((noinline)) void div_s(_Complex short *pa, _Complex short *pb, _Complex short *pc) { *pc = *pa / *pb; }
__attribute__((noinline)) void add_c(_Complex char *pa, _Complex char *pb, _Complex char *pc) { *pc = *pa + *pb; }
__attribute__((noinline)) void sub_c(_Complex char *pa, _Complex char *pb, _Complex char *pc) { *pc = *pa - *pb; }
__attribute__((noinline)) void add_uc(_Complex unsigned char *pa, _Complex unsigned char *pb, _Complex unsigned char *pc) { *pc = *pa + *pb; }
__attribute__((noinline)) void add_us(_Complex unsigned short *pa, _Complex unsigned short *pb, _Complex unsigned short *pc) { *pc = *pa + *pb; }
__attribute__((noinline)) void add_i(_Complex int *pa, _Complex int *pb, _Complex int *pc) { *pc = *pa + *pb; }
__attribute__((noinline)) void mul_i(_Complex int *pa, _Complex int *pb, _Complex int *pc) { *pc = *pa * *pb; }
__attribute__((noinline)) int eq_s(_Complex short *pa, _Complex short *pb) { return *pa == *pb; }
__attribute__((noinline)) int ne_s(_Complex short *pa, _Complex short *pb) { return *pa != *pb; }

int main(void)
{
  _Complex short a = 1 + 2i, b = 3 + 4i, c = 0;
  add_s(&a, &b, &c);
  printf("%d %d\n", (int)__real__ c, (int)__imag__ c);
  _Complex short na = -3 - 1000i, nb = -5 - 7i;
  add_s(&na, &nb, &c);
  printf("%d %d\n", (int)__real__ c, (int)__imag__ c);
  sub_s(&a, &b, &c);
  printf("%d %d\n", (int)__real__ c, (int)__imag__ c);
  mul_s(&a, &b, &c);
  printf("%d %d\n", (int)__real__ c, (int)__imag__ c);
  mul_s(&na, &b, &c);
  printf("%d %d\n", (int)__real__ c, (int)__imag__ c);
  _Complex short da = 1 + 7i, db = 1 + 1i;
  div_s(&da, &db, &c);
  printf("%d %d\n", (int)__real__ c, (int)__imag__ c);

  _Complex char x = 5 + 6i, y = 7 + 8i, z = 0;
  add_c(&x, &y, &z);
  printf("%d %d\n", (int)__real__ z, (int)__imag__ z);
  _Complex char nx = -100 - 50i, ny = -20 - 30i;
  sub_c(&nx, &ny, &z);
  printf("%d %d\n", (int)__real__ z, (int)__imag__ z);

  _Complex unsigned char u = 200 + 200i, v = 100 + 100i, w = 0;
  add_uc(&u, &v, &w);
  printf("%u %u\n", (unsigned)__real__ w, (unsigned)__imag__ w);

  _Complex unsigned short us = 65000 + 65000i, vs = 1000 + 1000i, ws = 0;
  add_us(&us, &vs, &ws);
  printf("%u %u\n", (unsigned)__real__ ws, (unsigned)__imag__ ws);

  _Complex int i1 = 100000 + 200000i, i2 = 300000 + 400000i, i3 = 0;
  add_i(&i1, &i2, &i3);
  printf("%d %d\n", (int)__real__ i3, (int)__imag__ i3);
  _Complex int mi1 = 1 + 2i, mi2 = 3 + 4i;
  mul_i(&mi1, &mi2, &i3);
  printf("%d %d\n", (int)__real__ i3, (int)__imag__ i3);

  _Complex short s = 1 + 2i, t = 0;
  t = s + 3;
  printf("%d %d\n", (int)__real__ t, (int)__imag__ t);
  t = 3 * s;
  printf("%d %d\n", (int)__real__ t, (int)__imag__ t);

  printf("%d %d\n", eq_s(&a, &a), eq_s(&a, &b));
  printf("%d %d\n", ne_s(&a, &a), ne_s(&a, &b));
  return 0;
}
