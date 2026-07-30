/* Guard for the pointer-to-pointer struct-copy inline path in vstore()
 * (tccgen.c): `*d = *s` for a word-aligned aggregate up to 16 bytes is
 * expanded to LOADs-then-STOREs instead of calling __aeabi_memmove4.
 *
 * The cap is 16 only when BOTH sides are register-deref lvalues; a LOCAL or
 * GLOBAL source stays at 8 because of the pr92618 store-forwarding
 * width-mismatch.  These cases check the results are byte-exact:
 *
 *   fwd/self   : the expansion loads everything before storing anything, so a
 *                fully-overlapping copy (d == s) and a copy between adjacent
 *                array elements must both come out right.
 *   from_local/from_global : the <=8 shapes that keep the old cap.
 *   partial    : 12 bytes, the size that used to go to the helper.
 */
#include <stdio.h>

struct T12 { int a, b, c; };
struct T16 { int a, b, c, d; };
struct T8 { int a, b; };

static struct T12 g12 = {101, 102, 103};
static struct T16 g16 = {201, 202, 203, 204};

__attribute__((noinline)) void cp12(struct T12 *d, const struct T12 *s) { *d = *s; }
__attribute__((noinline)) void cp16(struct T16 *d, const struct T16 *s) { *d = *s; }
__attribute__((noinline)) void cp8(struct T8 *d, const struct T8 *s) { *d = *s; }
__attribute__((noinline)) void from_local(struct T8 *d) { struct T8 t = {7, 8}; *d = t; }
__attribute__((noinline)) void from_global12(struct T12 *d) { *d = g12; }
__attribute__((noinline)) void from_global16(struct T16 *d) { *d = g16; }

int main(void)
{
  struct T12 a = {1, 2, 3}, b = {0, 0, 0};
  cp12(&b, &a);
  if (b.a != 1 || b.b != 2 || b.c != 3)
  {
    printf("FAIL cp12 %d %d %d\n", b.a, b.b, b.c);
    return 1;
  }
  /* Self-copy: dest and source are the same object. */
  cp12(&b, &b);
  if (b.a != 1 || b.b != 2 || b.c != 3)
  {
    printf("FAIL cp12 self %d %d %d\n", b.a, b.b, b.c);
    return 2;
  }

  struct T16 e = {11, 12, 13, 14}, f = {0, 0, 0, 0};
  cp16(&f, &e);
  if (f.a != 11 || f.b != 12 || f.c != 13 || f.d != 14)
  {
    printf("FAIL cp16 %d %d %d %d\n", f.a, f.b, f.c, f.d);
    return 3;
  }
  cp16(&f, &f);
  if (f.a != 11 || f.b != 12 || f.c != 13 || f.d != 14)
  {
    printf("FAIL cp16 self\n");
    return 4;
  }

  /* Adjacent array elements: distinct but neighbouring storage. */
  static struct T12 arr[3];
  arr[0].a = 21; arr[0].b = 22; arr[0].c = 23;
  arr[1].a = 31; arr[1].b = 32; arr[1].c = 33;
  cp12(&arr[1], &arr[0]);
  if (arr[1].a != 21 || arr[1].b != 22 || arr[1].c != 23 ||
      arr[0].a != 21 || arr[0].b != 22 || arr[0].c != 23)
  {
    printf("FAIL adjacent\n");
    return 5;
  }

  struct T8 g = {0, 0};
  cp8(&g, &(struct T8){41, 42});
  if (g.a != 41 || g.b != 42)
  {
    printf("FAIL cp8\n");
    return 6;
  }
  from_local(&g);
  if (g.a != 7 || g.b != 8)
  {
    printf("FAIL from_local\n");
    return 7;
  }

  struct T12 h = {0, 0, 0};
  from_global12(&h);
  if (h.a != 101 || h.b != 102 || h.c != 103)
  {
    printf("FAIL from_global12 %d %d %d\n", h.a, h.b, h.c);
    return 8;
  }
  struct T16 k = {0, 0, 0, 0};
  from_global16(&k);
  if (k.a != 201 || k.b != 202 || k.c != 203 || k.d != 204)
  {
    printf("FAIL from_global16\n");
    return 9;
  }

  printf("OK\n");
  return 0;
}
