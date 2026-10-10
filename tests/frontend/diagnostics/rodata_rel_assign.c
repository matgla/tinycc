static const char *__rodata_relative t[] = {"a"};
void f(void) { t[0] = "b"; }
