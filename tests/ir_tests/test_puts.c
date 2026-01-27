/* Test calling puts without stdio.h */

/* Declare puts manually - it's linked from libc */
extern int puts(const char *s);
extern int printf(const char *format, ...);

int main(void) {
    puts("Hello from puts!");
    printf("Printf works: %d\n", 42);
    return 0;
}
