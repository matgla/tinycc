/* Three independent references to the same external symbol should produce
 * three separate relocation entries, all indexing the same symbol-table
 * slot. */
extern int shared_var;

int use_a(void) {
  return shared_var;
}
int use_b(void) {
  return shared_var + 1;
}
int use_c(void) {
  return shared_var + 2;
}
