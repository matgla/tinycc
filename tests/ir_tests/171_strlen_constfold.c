/* Test compile-time constant folding of strlen() on string literals. */
#include <stdio.h>
#include <string.h>

volatile int sink;

int main()
{
    /* Direct string literal */
    printf("%d\n", (int)strlen("hello"));     /* 5 */
    printf("%d\n", (int)strlen(""));          /* 0 */
    printf("%d\n", (int)strlen("a"));         /* 1 */
    printf("%d\n", (int)strlen("hello world"));/* 11 */

    /* Through a const char pointer to a literal */
    const char *s = "test";
    printf("%d\n", (int)strlen(s));           /* 4 */

    /* Used in an expression */
    int len = strlen("abc") + strlen("de");
    printf("%d\n", len);                      /* 5 */

    /* Non-const - should NOT be folded but must still work */
    char buf[16];
    buf[0] = 'x';
    buf[1] = 'y';
    buf[2] = '\0';
    sink = buf[0]; /* prevent optimization of buf */
    printf("%d\n", (int)strlen(buf));         /* 2 */

    return 0;
}
