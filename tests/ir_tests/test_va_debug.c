#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

void myprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    
    // Print the va_list pointer value
    printf("va_list ap = %p\n", (void*)(uintptr_t)ap);
    printf("ap %% 8 = %d\n", (int)((uintptr_t)ap % 8));
    
    // Read raw bytes at ap and nearby
    uint32_t *p = (uint32_t*)ap;
    printf("ap[0] = 0x%08x\n", p[0]);
    printf("ap[1] = 0x%08x\n", p[1]);
    printf("ap[2] = 0x%08x\n", p[2]);
    printf("ap[3] = 0x%08x\n", p[3]);
    
    double d = va_arg(ap, double);
    uint32_t *dp = (uint32_t*)&d;
    printf("va_arg got: lo=0x%08x hi=0x%08x val=%f\n", dp[0], dp[1], d);
    va_end(ap);
}

int main() {
    myprintf("test", 2.6);
    return 0;
}
