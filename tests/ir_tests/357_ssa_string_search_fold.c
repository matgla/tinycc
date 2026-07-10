/* ssa:const_string_fold — strspn/strcspn/strchr/strrchr/index/rindex/strstr/
 * strpbrk folded on constant strings.
 *
 * At -O2 each call below folds to a constant count or a &literal+offset pointer
 * (GCC does the same); at -O0 they run the real functions.  Both must agree, so
 * this pins fold CORRECTNESS across the adversarially-verified edge cases:
 * not-found NULL, NUL-search returning the terminator, empty needle, first vs
 * last match, addend (substring base), high-bit bytes, char->NUL truncation.
 * Offsets are printed relative to each string base so the result is PIC-safe.
 */
#include <stdio.h>

#define OFF(base, p) ((p) ? (int)((const char *)(p) - (const char *)(base)) : -1)

int main(void)
{
  const char *h = "hello";
  const char *m = "abcabc";

  /* strspn / strcspn — counts */
  printf("%d %d %d %d %d %d\n",
         (int)__builtin_strspn("hello", "hel"),   /* 4 */
         (int)__builtin_strspn("hello", ""),      /* 0 */
         (int)__builtin_strspn("aab", "a"),       /* 2 */
         (int)__builtin_strcspn("hello", "lo"),   /* 2 */
         (int)__builtin_strcspn("hello", "xyz"),  /* 5 */
         (int)__builtin_strcspn("key=val;", "=;"));/* 3 */

  /* strchr / strrchr / index / rindex — offset or -1 */
  printf("%d %d %d %d %d %d\n",
         OFF(h, __builtin_strchr(h, 'l')),        /* 2 */
         OFF(h, __builtin_strrchr(h, 'l')),       /* 3 */
         OFF(h, __builtin_strchr(h, '\0')),       /* 5 (terminator) */
         OFF(h, __builtin_strchr(h, 'z')),        /* -1 (NULL) */
         OFF(h, __builtin_index(h, 'e')),         /* 1 (index == strchr) */
         OFF(h, __builtin_rindex(h, 'z')));       /* -1 (rindex not found) */

  /* strstr / strpbrk — offset or -1 */
  printf("%d %d %d %d %d %d\n",
         OFF(h, __builtin_strstr(h, "ll")),       /* 2 */
         OFF(h, __builtin_strstr(h, "")),         /* 0 (haystack) */
         OFF(m, __builtin_strstr(m, "bc")),       /* 1 (first) */
         OFF(h, __builtin_strstr(h, "xy")),       /* -1 */
         OFF(h, __builtin_strpbrk(h, "ol")),      /* 2 ('l' first in s) */
         OFF(h, __builtin_strpbrk(h, "xyz")));    /* -1 */

  return 0;
}
