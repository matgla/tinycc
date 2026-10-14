/* A -shared library built with -fvisibility=hidden exports only what it
 * marks visibility("default"); the rest of its globals are reached locally,
 * through the GOT or directly, never through a dynamic symbol. */
int lib_counter;
int (*lib_hook)(int);

int lib_helper(int x) { return x + lib_counter; }

__attribute__((visibility("default"))) int lib_api(int x)
{
  lib_counter++;
  lib_hook = lib_helper;
  return lib_hook(x);
}
