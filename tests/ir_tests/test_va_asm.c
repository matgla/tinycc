#include <stdarg.h>
void test(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    double d = va_arg(ap, double);
    va_end(ap);
    (void)d;
}
int main() { return 0; }
