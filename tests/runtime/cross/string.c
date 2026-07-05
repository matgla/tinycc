/* Force references to string helpers with -fno-builtin. */
char buf[64];
char src[64];

void force_string(void) {
    __builtin_memcpy(buf, src, 32);
    __builtin_memset(buf, 0, 32);
}
