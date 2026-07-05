/* __attribute__((weak)) on data and function definitions must produce
 * STB_WEAK symbols. */
__attribute__((weak)) int weak_var = 3;
__attribute__((weak)) int weak_func(void) {
  return 1;
}
