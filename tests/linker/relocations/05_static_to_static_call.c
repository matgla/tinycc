/* A direct call between two file-local (static) functions still emits a
 * THM_JUMP24 relocation against the callee's local symbol; tcc does not
 * patch the branch displacement directly even though both functions live
 * in the same object, so the linker/relaxation pass can still see it.
 * noinline keeps both calls from being inlined away by -O1. */
__attribute__((noinline)) static int helper(int x) {
  return x * 2 + 7;
}

__attribute__((noinline)) static int wrapper(int x) {
  return helper(x) + helper(x + 1);
}

int use(int x) {
  return wrapper(x);
}
