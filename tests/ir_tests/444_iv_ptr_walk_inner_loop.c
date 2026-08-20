/* IV strength reduction over INDEXED loads/stores, in an INNER loop.
 *
 * Two gates used to keep this transform away from every array walk that
 * matters:
 *   - transform_derived_iv refused any DIV whose use is a LOAD_INDEXED /
 *     STORE_INDEXED, on the premise that the backend's indexed addressing was
 *     already efficient.  Measured on the RP2350 Cortex-M33 (hand-written asm,
 *     256-word walk, cycles per element): `ldr.w r3,[base,i,lsl #2]` + index
 *     bump = 8.012, `ldr r3,[p]` + pointer bump = 7.012.  A scaled register
 *     offset costs a full extra cycle at equal instruction count, so the
 *     premise was backwards.
 *   - the driver considered OUTERMOST loops only, and a hot array walk is
 *     nearly always nested (`for (n) for (j) sum += a[j];`).
 *
 * The cases below pin the transform's correctness, not its shape, so they
 * stay meaningful if the heuristics change: each one produces a wrong sum if
 * the pointer walk desynchronises from the index it replaced.
 *
 *   inner      — the plain nested case the driver now reaches.
 *   param_base — base is an incoming argument (a PARAM has no defining quad,
 *                which the base-availability scan used to read as "not
 *                defined before the loop").
 *   guarded    — the indexed access sits under an `if`, so the pointer must
 *                not be advanced by the access itself.
 *   early_out  — a `break` leaves the loop mid-iteration.
 *   backward   — negative step: the end pointer is below the start.
 *   stride2    — step 2, so pointer stride is 8 while the index still moves 1
 *                per test.
 *   store      — STORE_INDEXED rather than LOAD_INDEXED.
 *   bytes      — stride 1 off a STACK base: `base + i` with no shift at all,
 *                which no other DIV pass can see (they key on a SHL/MUL) and
 *                which the indexed-memory fusion refuses for a local base, so
 *                the address genuinely stays in the loop.  This is
 *                bench_memcpy's checksum loop (-33% once transformed).
 *   bytes_off  — the same, read back through a second pointer so the walk has
 *                to stay in step with an offset access.
 *   scaled_shared — a scaled address used TWICE per iteration.  Scaled
 *                addresses are deliberately NOT strength-reduced (the shift
 *                folds into the addressing mode anyway, and doing it anyway
 *                cost mibench_stringsearch +12.4%); this case exists so that
 *                policy cannot silently change the answer.
 */
#include <stdio.h>

static int arr[64];
static int out[64];

static int inner(int reps)
{
  int total = 0;
  for (int n = 0; n < reps; n++)
  {
    int s = 0;
    for (int j = 0; j < 64; j++)
      s += arr[j];
    total += s;
  }
  return total;
}

static int param_base(const int *a, int reps)
{
  int total = 0;
  for (int n = 0; n < reps; n++)
    for (int j = 0; j < 64; j++)
      total += a[j];
  return total;
}

static int guarded(void)
{
  int s = 0;
  for (int n = 0; n < 3; n++)
    for (int j = 0; j < 64; j++)
      if (j & 1)
        s += arr[j];
  return s;
}

static int early_out(void)
{
  int s = 0;
  for (int n = 0; n < 3; n++)
    for (int j = 0; j < 64; j++)
    {
      if (arr[j] > 100)
        break;
      s += arr[j];
    }
  return s;
}

static int backward(void)
{
  int s = 0;
  for (int n = 0; n < 2; n++)
    for (int j = 63; j >= 0; j--)
      s += arr[j];
  return s;
}

static int stride2(void)
{
  int s = 0;
  for (int n = 0; n < 2; n++)
    for (int j = 0; j < 64; j += 2)
      s += arr[j];
  return s;
}

static int store(void)
{
  int s = 0;
  for (int n = 0; n < 2; n++)
  {
    for (int j = 0; j < 64; j++)
      out[j] = arr[j] * 2 + n;
    for (int j = 0; j < 64; j++)
      s += out[j];
  }
  return s;
}

static int bytes(void)
{
  unsigned char buf[256];
  int s = 0;
  for (int i = 0; i < 256; i++)
    buf[i] = (unsigned char)((i * 7 + 13) & 0xFF);
  for (int n = 0; n < 3; n++)
    for (int j = 0; j < 256; j++)
      s += buf[j];
  return s;
}

static int bytes_off(void)
{
  unsigned char buf[128];
  int s = 0;
  for (int i = 0; i < 128; i++)
    buf[i] = (unsigned char)(i + 1);
  for (int n = 0; n < 2; n++)
    for (int j = 0; j < 127; j++)
      s += buf[j] * 2 + buf[j + 1];
  return s;
}

static int scaled_shared(void)
{
  int s = 0;
  for (int n = 0; n < 2; n++)
    for (int j = 0; j < 64; j++)
      s += arr[j] + arr[j] / 2;
  return s;
}

int main(void)
{
  for (int i = 0; i < 64; i++)
    arr[i] = i * 3 + 1;

  printf("inner=%d param=%d guard=%d early=%d back=%d s2=%d store=%d\n", inner(3), param_base(arr, 3), guarded(),
         early_out(), backward(), stride2(), store());
  printf("bytes=%d bytes_off=%d shared=%d\n", bytes(), bytes_off(), scaled_shared());
  return 0;
}
