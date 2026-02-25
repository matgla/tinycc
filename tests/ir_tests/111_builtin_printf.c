int main(void)
{
    int ret;
    
    ret = __builtin_printf("Hello from __builtin_printf!\n");
    __builtin_printf("Return value: %d\n", ret);
    
    ret = __builtin_printf("Multiple args: %d, %s, %c\n", 42, "test", 'X');
    __builtin_printf("Return value: %d\n", ret);
    
    return 0;
}
