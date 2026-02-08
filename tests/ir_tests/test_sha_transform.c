/*
 * Minimal SHA-1 transform test: isolates sha_transform correctness.
 */
#include <stdio.h>
#include <string.h>

typedef unsigned char BYTE;
typedef unsigned long LONG;

#define SHA_BLOCKSIZE 64

typedef struct
{
  LONG digest[5];
  LONG count_lo, count_hi;
  LONG data[16];
} SHA_INFO;

#define f1(x, y, z) ((x & y) | (~x & z))
#define f2(x, y, z) (x ^ y ^ z)
#define f3(x, y, z) ((x & y) | (x & z) | (y & z))
#define f4(x, y, z) (x ^ y ^ z)

#define CONST1 0x5a827999L
#define CONST2 0x6ed9eba1L
#define CONST3 0x8f1bbcdcL
#define CONST4 0xca62c1d6L

#define ROT32(x, n) ((x << n) | (x >> (32 - n)))

#define FUNC(n, i)                                                                                                     \
  temp = ROT32(A, 5) + f##n(B, C, D) + E + W[i] + CONST##n;                                                            \
  E = D;                                                                                                               \
  D = C;                                                                                                               \
  C = ROT32(B, 30);                                                                                                    \
  B = A;                                                                                                               \
  A = temp

static void sha_transform(SHA_INFO *sha_info)
{
  int i;
  LONG temp, A, B, C, D, E, W[80];

  for (i = 0; i < 16; ++i)
  {
    W[i] = sha_info->data[i];
  }
  for (i = 16; i < 80; ++i)
  {
    W[i] = W[i - 3] ^ W[i - 8] ^ W[i - 14] ^ W[i - 16];
  }
  A = sha_info->digest[0];
  B = sha_info->digest[1];
  C = sha_info->digest[2];
  D = sha_info->digest[3];
  E = sha_info->digest[4];

  for (i = 0; i < 20; ++i)
  {
    FUNC(1, i);
  }
  for (i = 20; i < 40; ++i)
  {
    FUNC(2, i);
  }
  for (i = 40; i < 60; ++i)
  {
    FUNC(3, i);
  }
  for (i = 60; i < 80; ++i)
  {
    FUNC(4, i);
  }

  sha_info->digest[0] += A;
  sha_info->digest[1] += B;
  sha_info->digest[2] += C;
  sha_info->digest[3] += D;
  sha_info->digest[4] += E;
}

int main(void)
{
  SHA_INFO info;
  /* Standard SHA-1 initial values */
  info.digest[0] = 0x67452301L;
  info.digest[1] = 0xefcdab89L;
  info.digest[2] = 0x98badcfeL;
  info.digest[3] = 0x10325476L;
  info.digest[4] = 0xc3d2e1f0L;

  /* Fill data with known pattern (simulate byte-reversed "Hello...") */
  for (int i = 0; i < 16; i++)
  {
    info.data[i] = (LONG)(i * 0x01020304 + 0x41);
  }

  sha_transform(&info);

  printf("d0=%08lx d1=%08lx d2=%08lx d3=%08lx d4=%08lx\n", info.digest[0], info.digest[1], info.digest[2],
         info.digest[3], info.digest[4]);

  return 0;
}
