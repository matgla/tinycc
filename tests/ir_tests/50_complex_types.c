#include <stdio.h>

int main(void)
{
    _Complex float cf;
    _Complex double cd;
    
    /* Check sizes */
    if (sizeof(cf) != 8) {
        printf("FAIL: sizeof(_Complex float) = %d, expected 8\n", (int)sizeof(cf));
        return 1;
    }
    if (sizeof(cd) != 16) {
        printf("FAIL: sizeof(_Complex double) = %d, expected 16\n", (int)sizeof(cd));
        return 1;
    }
    
    /* Check that we can declare and use variables */
    printf("OK: Complex types work!\n");
    printf("sizeof(_Complex float) = %d\n", (int)sizeof(cf));
    printf("sizeof(_Complex double) = %d\n", (int)sizeof(cd));
    
    return 0;
}
