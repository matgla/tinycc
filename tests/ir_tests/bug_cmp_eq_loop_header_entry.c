/* Regression: ssa_opt_cmp_eq_prop pushed an equality fact derived from a loop
 * back-edge into the loop header's dominator subtree when the loop header is
 * also the function entry block (its only recorded predecessor is the
 * back-edge, since the implicit program-entry edge is not in the CFG).  That
 * folded the in-loop `if (c1 != c2) return ...;` compare to "always equal",
 * so a case-insensitive compare always reported "equal".  See strncasecmp.
 * Fix: only push the edge fact when the sole predecessor is the block's idom.
 */
int cmp(const char *s1, const char *s2, unsigned long n)
{
  while (n-- && *s1 && *s2) {
    char c1 = *s1++;
    char c2 = *s2++;
    if (c1 >= 'A' && c1 <= 'Z') c1 += 'a' - 'A';
    if (c2 >= 'A' && c2 <= 'Z') c2 += 'a' - 'A';
    if (c1 != c2) return c1 - c2;
  }
  return 0;
}

int main(void)
{
  __builtin_printf("eq=%d\n", cmp("PID", "PID", 3) == 0);     /* 1 */
  __builtin_printf("ne=%d\n", cmp("PID", "PPID", 3) != 0);    /* 1 */
  __builtin_printf("ci=%d\n", cmp("pid", "PID", 3) == 0);     /* 1 */
  __builtin_printf("df=%d\n", cmp("AB", "AC", 2) != 0);       /* 1 */
  return 0;
}
