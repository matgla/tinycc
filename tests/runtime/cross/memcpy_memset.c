/* Force references to plain memcpy/memset with builtins disabled. */
char buf[64];
char src[64];

int force_memcpy_memset(void) {
    memcpy(buf, src, 32);
    memset(buf, 0, 32);
    return buf[0];
}
