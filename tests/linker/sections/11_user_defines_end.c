/* An input that defines a name the linker would provide keeps its own
 * definition and visibility: tcc's boundary symbols are hidden, and the
 * visibility merge must not hide the program's _end along with them. */
int _end = 42;
__attribute__((visibility("default"))) int read_end(void) { return _end; }
