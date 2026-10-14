/* __attribute__((visibility("hidden"))) must mark the symbol STV_HIDDEN
 * while remaining STB_GLOBAL (hidden is a visibility, not a binding).
 * The whole suite compiles with -fvisibility=hidden (see _base_cflags() in
 * test_linker.py), so the plain global is hidden too; the explicit
 * visibility("default") one is the only global left visible. */
__attribute__((visibility("hidden"))) int hidden_var = 5;
__attribute__((visibility("hidden"))) int hidden_func(void) {
  return 4;
}

int plain_global = 6;

__attribute__((visibility("default"))) int default_global = 7;
