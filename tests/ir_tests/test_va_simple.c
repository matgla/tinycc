#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>

void myprintf(const char *fmt, ...) {
    void *fp;
    __asm__ __volatile__("mov %0, r7" : "=r"(fp));
    printf("FP = %p\n", fp);
    printf("&fmt = %p (FP%+d)\n", (void*)&fmt, (int)((char*)&fmt - (char*)fp));
    printf("fmt = %p\n", (void*)fmt);
}

int main() {
    myprintf("test");
    return 0;
}
