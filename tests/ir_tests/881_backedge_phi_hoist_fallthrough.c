/* ra:backedge_phi_hoist rewrites
 *     CMP; JUMPIF cc -> X; copies; JUMP back; ...; X:
 * into
 *     CMP; copies; JUMPIF !cc -> back
 * which is only correct when X is the fall-through after the JUMP.  A switch
 * whose `case 2:` jumps forward over the `case 1:` store, with the default arm
 * looping back through phi copies, put live code between the JUMP and X: the
 * not-taken path then ran the case-1 store on the case-2 path.
 *
 * f_while is the plain loop (ssa:loop_header_dup rotates it into the shape);
 * f_goto is the same loop rotated by hand (miscompiled even without that pass).
 */
#include <stdio.h>
#include <stdint.h>

int32_t bound = 9;
int arr[16];

__attribute__((noinline)) int f_while(int a0, int a1)
{
  int v0 = 100;
  unsigned guard1 = 0, guard2 = 0;
  while (a1 != bound) {
    if (++guard1 > 38)
      break;
    switch (a0 & 7) {
    case 1:
      arr[12] = v0;
    case 2:
      v0 += 1;
    }
  }
  for (;;) {
    if (++guard2 > 15)
      break;
  }
  return v0;
}

__attribute__((noinline)) int f_goto(int a0, int a1)
{
  int v0 = 100;
  unsigned guard1 = 0, guard2 = 0;
top:
  if (a1 == bound)
    goto out;
body:
  if (++guard1 > 38)
    goto out;
  switch (a0 & 7) {
  case 1:
    arr[12] = v0;
  case 2:
    v0 += 1;
    if (a1 != bound)
      goto body;
    goto out;
  }
  goto top;
out:
  for (;;) {
    if (++guard2 > 15)
      break;
  }
  return v0;
}

/* Same hazard with a store that must stay on one switch arm only and a
 * value carried around the default back-edge. */
__attribute__((noinline)) unsigned f_carry(unsigned n, unsigned sel, unsigned *out)
{
  unsigned acc = 1, prev = 0, i = 0;
  while (i != n) {
    unsigned t = acc;
    i++;
    switch (sel & 3) {
    case 1:
      out[i & 7] = acc;
    case 2:
      acc = acc * 3 + prev;
      prev = t;
      break;
    default:
      prev = acc;
      acc = t + i;
      continue;
    }
    acc ^= i;
  }
  return acc + prev;
}

int main(void)
{
  static const int a0s[] = {0, 1, 2, 3, 9, 10, 17, 18, -1, -6};
  static const int a1s[] = {9, 17, 0, -9};
  for (unsigned i = 0; i < sizeof a0s / sizeof a0s[0]; i++) {
    for (unsigned j = 0; j < sizeof a1s / sizeof a1s[0]; j++) {
      arr[12] = 0;
      int r = f_while(a0s[i], a1s[j]);
      printf("w %d %d: %d %d\n", a0s[i], a1s[j], r, arr[12]);
      arr[12] = 0;
      r = f_goto(a0s[i], a1s[j]);
      printf("g %d %d: %d %d\n", a0s[i], a1s[j], r, arr[12]);
    }
  }
  for (unsigned sel = 0; sel < 4; sel++) {
    for (unsigned n = 0; n < 6; n += 1) {
      unsigned out[8] = {0};
      unsigned r = f_carry(n, sel, out);
      unsigned h = 0;
      for (int k = 0; k < 8; k++)
        h = h * 31 + out[k];
      printf("c %u %u: %u %u\n", sel, n, r, h);
    }
  }
  return 0;
}
