/* Two post-allocation frame passes, once frame_dfe has replaced dead struct
 * copies by a few word moves (no Addr[StackLoc] left to make them bail):
 *  - ra:dead_frame_store's walk to the return must read an MLA
 *    accumulator (operand slot 3) like any other source: the store a
 *    rotated struct's word ends with is read only as `a * 100 + b`;
 *  - self_store may drop `T <- [s] ... [s] <- T`, but not the load when
 *    reload elimination dropped a later reload of [s] because T's register
 *    already held it: that register is read under another name. */
#include <stdio.h>
#include <string.h>

struct big
{
  int f[30];
};

static const struct big und = {{-1,  -2,  -3,  -4,  -5,  -6,  -7,  -8,  -9,  -10, -11, -12, -13, -14, -15,
                                -16, -17, -18, -19, -20, -21, -22, -23, -24, -25, -26, -27, -28, -29, -30}};
static const struct big img = {{1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                                16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30}};

__attribute__((noinline)) static int swap(int k)
{
  struct big a = img, b = und, t;
  a.f[0] = k;
  t = a;
  a = b;
  b = t;
  return a.f[0] * 100 + b.f[0] + a.f[29] + b.f[29];
}

struct B
{
  int f[12];
};

struct L
{
  long long q[6];
};

static const struct B img0 = {{-19, -70, -21, -54, -45, -11, -70, 13, 16, 34, -29, 14}};
static const struct B img1 = {{-57, -20, -29, 28, 59, -100, 76, 31, -89, -68, -42, 12}};
static const struct B img2 = {{-61, 52, 7, -40, -32, -35, 69, 11, -100, 41, -49, 69}};

__attribute__((noinline)) static unsigned rotate(unsigned k, unsigned j)
{
  struct B a = img0, b = img1, c = img2, t;
  struct L l;
  unsigned r = 0;
  (void)k;
  (void)j;
  memset(&t, 0, sizeof t);
  memset(&l, 0, sizeof l);
  r ^= b.f[3];
  b = c;
  c = a;
  a = b;
  t = c;
  c = a;
  a = t;
  t = img1; /* dead: never read before the next write */
  b = a;
  a = c;
  c = b;
  a.f[10] = (unsigned)l.q[4];
  t = b;
  b = c;
  c = t;
  return t.f[7] * 111 + t.f[6] + c.f[3] * 160 + r;
}

int main(void)
{
  printf("swap %d %d\n", swap(3), swap(-7));
  printf("rotate %u %u\n", rotate(1, 4), rotate(2, 0));
  return 0;
}
