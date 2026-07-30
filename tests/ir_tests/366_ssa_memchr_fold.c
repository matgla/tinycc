/* ssa:const_string_fold — memchr folded on constant strings (str_memchr.c).
 *
 * At -O2 the const cases fold to &literal+offset or NULL; at -O0 the real
 * memchr runs.  Both must agree, so this pins fold CORRECTNESS on the edges:
 * not-found NULL, match beyond n, the terminator as a valid target (n ==
 * strlen+1), an index-computed base (only the SSA pass can reach that one), and
 * n running past the NUL — which must NOT fold, since the pass only knows the
 * bytes up to the terminator.  Offsets are printed relative to each base so the
 * result is PIC-safe.
 */
#include <stdio.h>

#define OFF(base, p) ((p) ? (int)((const char *)(p) - (const char *)(base)) : -1)

int main(void)
{
  const char *h = "hello world";
  static const char emb[] = "ab\0cd"; /* bytes past the NUL are known only at runtime */
  int i = 1;
  int j = i + 3;

  printf("%d %d %d %d\n",
         OFF(h, __builtin_memchr(h, 'o', 8)),    /* 4 */
         OFF(h, __builtin_memchr(h, 'z', 5)),    /* -1 (absent) */
         OFF(h, __builtin_memchr(h, '\0', 12)),  /* 11 (terminator, n == strlen+1) */
         OFF(h, __builtin_memchr(h, 'w', 6)));   /* -1 ('w' sits past n) */

  printf("%d %d\n",
         OFF(emb, __builtin_memchr(emb, 'c', 6)),  /* 3 — past the NUL, must not fold */
         OFF(emb, __builtin_memchr(emb, 'b', 2))); /* 1 */

  /* Index-computed base: a runtime address until SSA address folding. */
  printf("%d %d\n",
         OFF(h, __builtin_memchr(h + j, 'o', 4)),  /* 4 ("o world" -> +0) */
         OFF(h, __builtin_memchr(h + j, 'z', 4))); /* -1 */

  return 0;
}
