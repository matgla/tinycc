/* Hard-float: complex division goes through libgcc's __divsc3/__divdc3, which
 * under -mfloat-abi=hard take s0-s3 / d0-d3 and return the result in s0,s1 /
 * d0,d1.  The caller must also keep the (unmodelled) VFP clobbers harmless and
 * save LR even when the function looks like a leaf.  A _Complex float returned
 * by a call (d0) must be usable as an operand.  Returns 1 on success. */
__attribute__((noinline)) _Complex float cmkf(float r, float i)
{
  _Complex float z;
  __real__ z = r;
  __imag__ z = i;
  return z;
}

/* Only complex ops, no explicit call: the function must still save LR. */
__attribute__((noinline)) _Complex float divf(_Complex float a, _Complex float b) { return a / b; }
__attribute__((noinline)) _Complex double divd(_Complex double a, _Complex double b) { return a / b; }

__attribute__((noinline)) float live_across(float x, float y, _Complex float a, _Complex float b)
{
  float t = x * y + x; /* lives in a VFP register across the division */
  _Complex float q = a / b;
  return t + __real__ q + 10.0f * __imag__ q;
}

int main(void)
{
  volatile float one = 1.0f, two = 2.0f;
  _Complex float a = cmkf(one * 4.0f, two * 3.0f); /* 4+6i */
  _Complex float b = cmkf(one * 2.0f, two * 1.0f); /* 2+2i */
  _Complex float d = divf(a, b);                   /* (4+6i)/(2+2i) = 2.5+0.5i */
  if (__real__ d != 2.5f || __imag__ d != 0.5f)
    return 10;

  _Complex float s = cmkf(10.0f, 20.0f) - cmkf(one, two); /* call results as operands */
  if (__real__ s != 9.0f || __imag__ s != 18.0f)
    return 11;

  volatile double dr = 4.0, di = 6.0, er = 2.0, ei = 2.0;
  _Complex double da = dr + di * 1.0i, db = er + ei * 1.0i;
  _Complex double dd = divd(da, db);
  if (__real__ dd != 2.5 || __imag__ dd != 0.5)
    return 12;

  /* t = 3*2+3 = 9; q = 2.5+0.5i -> 9 + 2.5 + 5 = 16.5 */
  if (live_across(3.0f, 2.0f, a, b) != 16.5f)
    return 13;
  return 1;
}
