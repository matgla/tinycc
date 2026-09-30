/* Binding coverage: static (file-local) data/functions are STB_LOCAL;
 * plain globals are STB_GLOBAL.  The statics are marked used: nothing refers
 * to them, and an unused static is not emitted at all (-fdrop-unused-statics). */
__attribute__((used)) static int static_var = 1;
int global_var = 2;

__attribute__((used)) static int static_func(void) {
  return 1;
}
int global_func(void) {
  return 2;
}
