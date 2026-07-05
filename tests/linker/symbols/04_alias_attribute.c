/* __attribute__((alias("target"))) creates a second global symbol at the
 * exact same value/size as the target. */
int real_func(int x) {
  return x + 1;
}
int alias_func(int x) __attribute__((alias("real_func")));
