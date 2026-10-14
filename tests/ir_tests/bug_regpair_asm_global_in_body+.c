/* The other TU of bug_regpair_asm_global_in_body.c: f, g and h by name. */
typedef struct { int a, b; } P;
P f(int x);
P g(int x);
P h(int x);
P call_f(int x) { return f(x); }
P call_g(int x) { return g(x); }
P call_h(int x) { return h(x); }
