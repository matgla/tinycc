/* Force references to the alloca runtime helper. */
extern void *alloca(unsigned int size);

int force_vla(int n) {
    char *p = (char *)alloca((unsigned int)n);
    p[0] = 1;
    return p[0];
}
