static const char *__rodata_relative t[] = {"a"};
const char **f(void) { return &t[0]; }
