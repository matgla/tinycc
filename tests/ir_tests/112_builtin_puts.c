int main(void)
{
    int ret;
    
    // Test __builtin_puts with simple string
    ret = __builtin_puts("Hello from __builtin_puts!");
    __builtin_printf("Return value: %d\n", ret);
    
    // Test __builtin_puts return value (non-negative on success)
    if (ret >= 0) {
        __builtin_puts("SUCCESS");
    } else {
        __builtin_puts("FAILURE");
    }
    
    return 0;
}
