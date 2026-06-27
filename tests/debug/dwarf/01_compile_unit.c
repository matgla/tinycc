/* Minimal TU to inspect DWARF compile unit DIE. */
static int static_var = 42;
int global_var;

int compute(int x) {
    return x + static_var + global_var;
}
