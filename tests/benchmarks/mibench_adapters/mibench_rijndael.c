/*
 * MiBench Rijndael Adapter for RP2350 Benchmark Suite
 *
 * Encrypts and decrypts deterministic 128-bit blocks using the bundled AES code.
 */

#include "../benchmarks.h"
#include <string.h>

#include "../mibench/security/rijndael/aes.c"

static const byte rijndael_key[16] = {
    0x00, 0x11, 0x22, 0x33,
    0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xAA, 0xBB,
    0xCC, 0xDD, 0xEE, 0xFF,
};

int bench_mibench_rijndael(int iterations)
{
  aes context = {0};
  byte plain[16];
  byte encrypted[16];
  byte decrypted[16];
  int checksum = 0;

  if (set_key(rijndael_key, sizeof(rijndael_key), both, &context) != aes_good)
  {
    return -1;
  }

  for (int iteration = 0; iteration < iterations; iteration++)
  {
    for (int index = 0; index < 16; index++)
    {
      plain[index] = (byte)((index * 17 + iteration * 9 + 3) & 0xFF);
    }

    encrypt(plain, encrypted, &context);
    decrypt(encrypted, decrypted, &context);

    checksum = 0;
    for (int index = 0; index < 16; index++)
    {
      checksum += encrypted[index] * (index + 1);
      checksum += decrypted[index];
    }
  }

  return checksum;
}

void init_mibench_rijndael(void)
{
  register_benchmark_ex("mibench_rijndael", bench_mibench_rijndael, 300, "MiBench: Rijndael AES blocks", 18890);
}
