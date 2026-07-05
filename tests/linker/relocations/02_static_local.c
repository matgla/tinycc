/* Static data should be resolved without relocations in a single TU. */
static int static_var = 123;

int only_static(int x) {
    return x + static_var;
}
