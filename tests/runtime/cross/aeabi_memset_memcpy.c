/* Force references to the ARM EABI memory helpers. */
extern void *__aeabi_memcpy(void *dest, const void *src, unsigned long n);
extern void *__aeabi_memmove(void *dest, const void *src, unsigned long n);
extern void __aeabi_memset(void *dest, unsigned long n, int c);

char src[32];
char dst[32];

int force_aeabi_mem(void) {
    __aeabi_memset(dst, sizeof(dst), 0);
    __aeabi_memcpy(dst, src, sizeof(src));
    __aeabi_memmove(dst + 4, src, 16);
    return dst[0];
}
