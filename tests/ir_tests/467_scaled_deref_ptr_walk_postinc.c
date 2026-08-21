/* Scaled deref DIVs reduced to a pointer walk that ends post-indexed.
 *
 * A SCALED derived address that is dereferenced (`a[i] = v` through an
 * explicit SHL+ADD) used to be refused outright: the indexed-memory fusion
 * folds the shift into the addressing mode anyway, so the walk only traded
 * `str.w r,[base,i,lsl #2]` for `str r,[p]` + `adds p,#k` — measured a LOSS
 * (mibench_stringsearch +12.4%).  With ra:load_postinc / ra:store_postinc the
 * bump folds into the access itself (`str r,[p],#4`), dropping an instruction
 * per iteration AND the scaled index's +1 cycle, so
 * iv_scaled_deref_postinc_viable now admits exactly the shapes where that
 * fusion is assured: every use of the address is a plain 32-bit access with a
 * register value, at least one inside the loop, stride 1..255, counter
 * eliminable.
 *
 * These cases pin CORRECTNESS, not shape — each one produces a wrong value if
 * the walk desynchronises from the index, the write-back lands in the wrong
 * register, or the fused store writes the wrong slot.
 *
 *   fill         — the canonical admitted shape (global base, constant trip).
 *   fill_local   — the same off a stack base.
 *   rmw          — `g[i] = g[i] + 1`: TWO derefs of one address; only the
 *                  last access before the bump may take the write-back.
 *   fill_partial — the store sits under a data-dependent `if`, so folding the
 *                  bump into it would skip increments; must stay correct
 *                  whether the gate admits or refuses.
 *   fill_short   — INT16 access: outside the admitted widths, and a fused
 *                  STRH must not become a word store if policy ever changes.
 *   big_stride   — 260-byte elements: beyond the post-index immediate range.
 *   readback     — store then reload the same element in one iteration, so
 *                  the fused write-back must not outrun the reload.
 *   chained      — two arrays walked off the same index in one loop.
 */
#include <stdio.h>

static int g[256];
static short h[64];

struct big
{
  int v;
  char pad[256];
};
static struct big bigs[8];

static void fill(int v)
{
  for (int i = 0; i < 256; i++)
    g[i] = v;
}

static int fill_local(void)
{
  int loc[64];
  int s = 0;
  for (int i = 0; i < 64; i++)
    loc[i] = i * 5 + 2;
  for (int i = 0; i < 64; i++)
    s += loc[i];
  return s;
}

static void rmw(void)
{
  for (int i = 0; i < 256; i++)
    g[i] = g[i] + 1;
}

static void fill_partial(int v)
{
  for (int i = 0; i < 256; i++)
    if (i & 1)
      g[i] = v;
}

static int fill_short(short v)
{
  int s = 0;
  for (int i = 0; i < 64; i++)
    h[i] = (short)(v + i);
  for (int i = 0; i < 64; i++)
    s += h[i];
  return s;
}

static int big_stride(void)
{
  int s = 0;
  for (int i = 0; i < 8; i++)
    bigs[i].v = i * 9 + 4;
  for (int i = 0; i < 8; i++)
    s += bigs[i].v;
  return s;
}

static int readback(void)
{
  int s = 0;
  for (int i = 0; i < 256; i++)
  {
    g[i] = i * 3;
    s += g[i];
  }
  return s;
}

static int chained(void)
{
  int a[32], b[32];
  int s = 0;
  for (int i = 0; i < 32; i++)
  {
    a[i] = i + 1;
    b[i] = i * 2;
  }
  for (int i = 0; i < 32; i++)
    s += a[i] * b[i];
  return s;
}

int main(void)
{
  int s;

  fill(7);
  s = 0;
  for (int i = 0; i < 256; i++)
    s += g[i];
  printf("fill=%d local=%d\n", s, fill_local());

  rmw();
  s = 0;
  for (int i = 0; i < 256; i++)
    s += g[i];
  printf("rmw=%d\n", s);

  fill(0);
  fill_partial(5);
  s = 0;
  for (int i = 0; i < 256; i++)
    s += g[i] * (i + 1);
  printf("partial=%d\n", s);

  printf("short=%d big=%d readback=%d chained=%d\n", fill_short(3), big_stride(), readback(), chained());
  return 0;
}
