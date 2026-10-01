/* Standard ELF symtab convention: all STB_LOCAL symbols must precede the
 * first non-local (global/weak) symbol, and the section header's sh_info
 * field must equal the index of that first non-local symbol. Interleave
 * local and global data/function definitions to check tcc groups them
 * correctly rather than preserving source order.  The statics are marked
 * used: an unused static is not emitted at all (-fdrop-unused-statics). */
int g1 = 1;
__attribute__((used)) static int s1 = 2;
int g2(void) {
  return 1;
}
__attribute__((used)) static int s2(void) {
  return 2;
}
int g3 = 3;
__attribute__((used)) static int s3 = 4;
