/* Test __builtin_bswap16, __builtin_bswap32, __builtin_bswap64 */
#include <stdio.h>
#include <stdint.h>

int main(void)
{
    int errors = 0;
    
    /* Test __builtin_bswap16 */
    {
        uint16_t a = 0x1234;
        uint16_t r = __builtin_bswap16(a);
        if (r != 0x3412) {
            printf("bswap16(0x%04X) = 0x%04X, expected 0x3412\n", a, r);
            errors++;
        }
        
        /* Test constant folding */
        uint16_t c = __builtin_bswap16(0xABCD);
        if (c != 0xCDAB) {
            printf("bswap16(0xABCD) = 0x%04X, expected 0xCDAB\n", c);
            errors++;
        }
    }
    
    /* Test __builtin_bswap32 */
    {
        uint32_t a = 0x12345678;
        uint32_t r = __builtin_bswap32(a);
        if (r != 0x78563412) {
            printf("bswap32(0x%08X) = 0x%08X, expected 0x78563412\n", a, r);
            errors++;
        }
        
        /* Test constant folding */
        uint32_t c = __builtin_bswap32(0xDEADBEEF);
        if (c != 0xEFBEADDE) {
            printf("bswap32(0xDEADBEEF) = 0x%08X, expected 0xEFBEADDE\n", c);
            errors++;
        }
    }
    
    /* Test __builtin_bswap64 */
    {
        uint64_t a = 0x0123456789ABCDEFULL;
        uint64_t r = __builtin_bswap64(a);
        if (r != 0xEFCDAB8967452301ULL) {
            printf("bswap64 failed: got wrong result\n");
            errors++;
        }
        
        /* Test constant folding */
        uint64_t c = __builtin_bswap64(0x1122334455667788ULL);
        if (c != 0x8877665544332211ULL) {
            printf("bswap64 constant folding failed\n");
            errors++;
        }
    }
    
    if (errors == 0) {
        printf("All bswap tests passed!\n");
        return 0;
    } else {
        printf("%d test(s) failed\n", errors);
        return 1;
    }
}
