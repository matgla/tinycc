/* Direct semihosting test - simpler version */

void print_char(int c) {
    __asm__ volatile (
        "mov r0, #3\n"     /* SYS_WRITEC */
        "mov r1, %0\n"
        "bkpt #0xab\n"
        : : "r"(&c) : "r0", "r1", "memory"
    );
}

void print_str(const char *str) {
    __asm__ volatile (
        "mov r0, #4\n"     /* SYS_WRITE0 */
        "mov r1, %0\n"
        "bkpt #0xab\n"
        : : "r"(str) : "r0", "r1", "memory"
    );
}

int main(void) {
    print_str("Hello from semihosting!\n");
    int x = 'X';
    print_char(x);
    int n = '\n';
    print_char(n);
    return 42;
}
