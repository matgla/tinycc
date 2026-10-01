int main(void)
{
    int ret;
    
    // Test __builtin_puts with simple string
    ret = __builtin_puts("Hello from __builtin_puts!");
    /* The value itself is the libc's: newlib's puts returns '\n' (10), YasOS
     * libc's the number of characters written (27). */
    __builtin_printf("Return value non-negative: %d\n", ret >= 0);
    
    // C only guarantees a non-negative return value on success.
    if (ret >= 0) {
        __builtin_puts("SUCCESS");
    } else {
        __builtin_puts("FAILURE");
    }
    
    return 0;
}
