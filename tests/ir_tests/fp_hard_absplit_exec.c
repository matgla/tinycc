/* Hard-float AAPCS-VFP: independent GPR / VFP / stack argument banks.
 *
 * mix6 takes 5 ints (r0-r3 + one on the stack) and 2 floats (s0, s1): the float
 * args must NOT disturb the 5th int's stack slot (the phantom-stack-store bug),
 * and the GPR/VFP counters must advance independently.  sum6f's 6 floats all
 * ride s0-s5.  interleave alternates int/float so each bank fills separately.
 * Returns 1 on success, else a diagnostic code. */

static int mix6(int a, int b, int c, int d, int e, float x, float y)
{
  return a + b + c + d + e + (int)(x * y);
}

static float sum6f(float a, float b, float c, float d, float e, float f)
{
  return a + b + c + d + e + f;
}

static int interleave(int a, float x, int b, float y, int c, float z)
{
  return a + b + c + (int)(x + y + z);
}

int main(void)
{
  /* 1+2+3+4+5 + (int)(2.5*4.0=10.0) = 25 */
  if (mix6(1, 2, 3, 4, 5, 2.5f, 4.0f) != 25)
    return 10;

  /* 21.0 */
  if ((int)sum6f(1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f) != 21)
    return 11;

  /* 1+2+3 + (int)(1.5+2.5+3.0=7.0) = 13 */
  if (interleave(1, 1.5f, 2, 2.5f, 3, 3.0f) != 13)
    return 12;

  return 1;
}
