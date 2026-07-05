/* Relocations for external global variables and function calls. */
extern int external_var;
extern int external_func(int);

int global_var;
static int static_var = 42;

int caller(int x) {
    return x + external_var + external_func(x) + static_var + global_var;
}
