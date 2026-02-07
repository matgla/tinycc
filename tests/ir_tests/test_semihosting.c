/* Direct semihosting test without stdio.h */

/* Semihosting operations */
#define SYS_WRITEC 0x03
#define SYS_WRITE0 0x04
#define SYS_EXIT 0x18

static void semihosting_writec(char c) {
    register int r0 asm("r0") = SYS_WRITEC;
    register const char *r1 asm("r1") = &c;
    asm volatile("bkpt #0xab" : : "r"(r0), "r"(r1) : "memory");
}

static void semihosting_write0(const char *str) {
    register int r0 asm("r0") = SYS_WRITE0;
    register const char *r1 asm("r1") = str;
    asm volatile("bkpt #0xab" : : "r"(r0), "r"(r1) : "memory");
}

int main(void) {
    semihosting_write0("Hello from semihosting!\n");
    semihosting_writec('X');
    semihosting_writec('\n');
    return 0;
}
