/* Phase D lever: wide-string-literal merge.
 * Two identical wide string literals are emitted separately today.  Once a
 * literal-pool merge pass lands the .rodata should contain only one copy.
 */
const int *f1(void) { return (const int *)L"abc"; }
const int *f2(void) { return (const int *)L"abc"; }
