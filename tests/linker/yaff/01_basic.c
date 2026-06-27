/* Minimal self-contained program for YAFF output inspection. */
static int static_var = 42;
int global_var = 7;

int func(int x) { return x + global_var + static_var; }

void _start(void) { func(0); }
