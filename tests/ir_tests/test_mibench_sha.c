/*
 * Test: MiBench SHA-1 adapted for QEMU ir_test harness.
 * Reproduces the -O1 crash observed on RP2350 benchmarks.
 */
#include <stdio.h>
#include <string.h>

typedef unsigned char BYTE;
typedef unsigned long LONG;

#define SHA_BLOCKSIZE 64

typedef struct {
    LONG digest[5];
    LONG count_lo, count_hi;
    LONG data[16];
} SHA_INFO;

/* SHA f()-functions */
#define f1(x,y,z) ((x & y) | (~x & z))
#define f2(x,y,z) (x ^ y ^ z)
#define f3(x,y,z) ((x & y) | (x & z) | (y & z))
#define f4(x,y,z) (x ^ y ^ z)

#define CONST1 0x5a827999L
#define CONST2 0x6ed9eba1L
#define CONST3 0x8f1bbcdcL
#define CONST4 0xca62c1d6L

#define ROT32(x,n) ((x << n) | (x >> (32 - n)))

#define FUNC(n,i) \
    temp = ROT32(A,5) + f##n(B,C,D) + E + W[i] + CONST##n; \
    E = D; D = C; C = ROT32(B,30); B = A; A = temp

static void sha_transform(SHA_INFO *sha_info)
{
    int i;
    LONG temp, A, B, C, D, E, W[80];

    for (i = 0; i < 16; ++i) {
        W[i] = sha_info->data[i];
    }
    for (i = 16; i < 80; ++i) {
        W[i] = W[i-3] ^ W[i-8] ^ W[i-14] ^ W[i-16];
    }
    A = sha_info->digest[0];
    B = sha_info->digest[1];
    C = sha_info->digest[2];
    D = sha_info->digest[3];
    E = sha_info->digest[4];

    for (i = 0; i < 20; ++i) { FUNC(1,i); }
    for (i = 20; i < 40; ++i) { FUNC(2,i); }
    for (i = 40; i < 60; ++i) { FUNC(3,i); }
    for (i = 60; i < 80; ++i) { FUNC(4,i); }

    sha_info->digest[0] += A;
    sha_info->digest[1] += B;
    sha_info->digest[2] += C;
    sha_info->digest[3] += D;
    sha_info->digest[4] += E;
}

static void byte_reverse(LONG *buffer, int count)
{
    int i;
    BYTE ct[4], *cp;
    count /= sizeof(LONG);
    cp = (BYTE *) buffer;
    for (i = 0; i < count; ++i) {
        ct[0] = cp[0]; ct[1] = cp[1]; ct[2] = cp[2]; ct[3] = cp[3];
        cp[0] = ct[3]; cp[1] = ct[2]; cp[2] = ct[1]; cp[3] = ct[0];
        cp += sizeof(LONG);
    }
}

void sha_init(SHA_INFO *sha_info)
{
    sha_info->digest[0] = 0x67452301L;
    sha_info->digest[1] = 0xefcdab89L;
    sha_info->digest[2] = 0x98badcfeL;
    sha_info->digest[3] = 0x10325476L;
    sha_info->digest[4] = 0xc3d2e1f0L;
    sha_info->count_lo = 0L;
    sha_info->count_hi = 0L;
}

void sha_update(SHA_INFO *sha_info, BYTE *buffer, int count)
{
    if ((sha_info->count_lo + ((LONG) count << 3)) < sha_info->count_lo) {
        ++sha_info->count_hi;
    }
    sha_info->count_lo += (LONG) count << 3;
    sha_info->count_hi += (LONG) count >> 29;
    while (count >= SHA_BLOCKSIZE) {
        memcpy(sha_info->data, buffer, SHA_BLOCKSIZE);
        byte_reverse(sha_info->data, SHA_BLOCKSIZE);
        sha_transform(sha_info);
        buffer += SHA_BLOCKSIZE;
        count -= SHA_BLOCKSIZE;
    }
    memcpy(sha_info->data, buffer, count);
}

void sha_final(SHA_INFO *sha_info)
{
    int count;
    LONG lo_bit_count, hi_bit_count;

    lo_bit_count = sha_info->count_lo;
    hi_bit_count = sha_info->count_hi;
    count = (int) ((lo_bit_count >> 3) & 0x3f);
    ((BYTE *) sha_info->data)[count++] = 0x80;
    if (count > 56) {
        memset((BYTE *) &sha_info->data + count, 0, 64 - count);
        byte_reverse(sha_info->data, SHA_BLOCKSIZE);
        sha_transform(sha_info);
        memset(&sha_info->data, 0, 56);
    } else {
        memset((BYTE *) &sha_info->data + count, 0, 56 - count);
    }
    byte_reverse(sha_info->data, SHA_BLOCKSIZE);
    sha_info->data[14] = hi_bit_count;
    sha_info->data[15] = lo_bit_count;
    sha_transform(sha_info);
}

static const char test_data[] = "The quick brown fox jumps over the lazy dog. "
                                "Pack my box with five dozen liquor jugs. "
                                "How vexingly quick daft zebras jump! "
                                "The five boxing wizards jump quickly. "
                                "Sphinx of black quartz, judge my vow.";

int main(void)
{
    SHA_INFO sha_info;
    int i, j;
    volatile int checksum = 0;

    char input_buffer[1024];
    int data_len = 0;

    while (data_len + (int)sizeof(test_data) < (int)sizeof(input_buffer)) {
        memcpy(input_buffer + data_len, test_data, sizeof(test_data) - 1);
        data_len += sizeof(test_data) - 1;
    }

    for (i = 0; i < 2; i++) {
        input_buffer[0] = (char)('A' + (i % 26));
        sha_init(&sha_info);
        sha_update(&sha_info, (BYTE *)input_buffer, data_len);
        sha_final(&sha_info);

        checksum = 0;
        for (j = 0; j < 5; j++) {
            checksum += (int)(sha_info.digest[j] & 0xFFFF);
        }
    }

    printf("SHA checksum: %d\n", checksum);
    return 0;
}
