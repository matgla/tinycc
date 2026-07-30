/* Hard-float (-mfloat-abi=hard -mfpu=fpv5-sp-d16) execution gate — calls:
 * float select (ternary), calls through a function pointer, and functions with
 * more than four float arguments (which overflow to the stack).  Returns 1 on
 * success, else a distinct diagnostic code.
 */

static float sel(int c, float a, float b) { return c ? a : b; }
static float dbl(float x) { return x + x; }
static float sum6(float a, float b, float c, float d, float e, float f)
{
  return a + b + c + d + e + f; /* 6 float args: 5th/6th passed on the stack */
}

typedef float (*fp_t)(float);

int main(void)
{
  float r = sel(1, 2.5f, 3.5f) + sel(0, 2.5f, 3.5f); /* 2.5 + 3.5 = 6.0 */
  if ((int)r != 6)
    return 10;

  fp_t fp = dbl;
  float t = fp(7.0f); /* 14.0 via function pointer */
  if ((int)t != 14)
    return 11;

  float s = sum6(1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f); /* 21.0, stack args */
  if ((int)s != 21)
    return 12;

  /* 6.0 + 14.0 + 21.0 = 41.0 */
  if ((int)(r + t + s) != 41)
    return 13;

  return 1;
}
