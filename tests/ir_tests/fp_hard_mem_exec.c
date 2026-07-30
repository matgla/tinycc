/* Hard-float (-mfloat-abi=hard -mfpu=fpv5-sp-d16) execution gate — memory,
 * arrays, globals, computed-float arguments, and float returns kept as float.
 * Exercises the VFP register class beyond simple arithmetic: float locals in
 * VFP registers spilled/reloaded through memory, stores to global floats and
 * float arrays, float pointer dereference, and passing a computed (VFP-resident)
 * float as an argument.  Returns 1 on success, else a distinct diagnostic code.
 */

float garr[4];
float gv;

static float sq(float x) { return x * x; }
static float use3(float a, float b, float c) { return a * b + c; }

int main(void)
{
  float local[3];
  local[0] = 1.5f;
  local[1] = 2.5f;
  local[2] = local[0] + local[1]; /* 4.0 */
  if ((int)local[2] != 4)
    return 10;

  gv = local[2] * 2.0f; /* 8.0, global store */
  garr[0] = gv;         /* float array store */
  if ((int)garr[0] != 8)
    return 11;

  float t = sq(gv); /* 64.0, float return kept as float */
  if ((int)t != 64)
    return 12;

  float u = use3(local[0], local[1], t); /* 1.5*2.5 + 64 = 67.75, computed args */
  if ((int)u != 67)
    return 13;

  float *p = &garr[0];
  *p = *p + u; /* 8.0 + 67.75 = 75.75, ptr deref store */
  if ((int)garr[0] != 75)
    return 14;

  /* 75.75 + 8.0 + 64.0 + 67.75 = 215.5 */
  if ((int)(garr[0] + gv + t + u) != 215)
    return 15;

  return 1;
}
