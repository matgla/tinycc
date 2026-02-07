/* Test calling puts with explicit flush */

/* Declare stdio functions manually */
extern int puts(const char *s);
extern int printf(const char *format, ...);
extern int fflush(void *stream);

int main(void) {
    puts("Hello from puts!");
    printf("Printf works: %d\n", 42);
    fflush(0);  /* Flush all streams */
    return 0;
}
