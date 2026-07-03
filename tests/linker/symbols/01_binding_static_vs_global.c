/* Binding coverage: static (file-local) data/functions are STB_LOCAL;
 * plain globals are STB_GLOBAL. */
static int static_var = 1;
int global_var = 2;

static int static_func(void) {
  return 1;
}
int global_func(void) {
  return 2;
}
