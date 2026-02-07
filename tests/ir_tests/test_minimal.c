void uart_write(const char *s) {
    // Dummy - just to get the pointer into a register
    volatile const char *p = s;
    (void)p;
}

const char *get_str(int i) {
    if (i == 1)
        return "HELLO";
    return "WORLD";
}

void _start(void) {
    uart_write(get_str(0));
    uart_write(get_str(1));
}
