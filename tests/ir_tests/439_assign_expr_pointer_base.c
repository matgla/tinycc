/* An assignment used as a sub-expression must not leave the assigned POINTER
 * resolving to the register that holds the enclosing expression's VALUE.
 *
 *     *(rr = *rd) = ++cnt;
 *     rr[2 * cnt - 1] = hfd;
 *
 * At -O2 both indexed stores were emitted with `cnt` as the base register
 * instead of `rr` -- `str.w r5, [r1, r2, lsl #2]` where r1 held cnt, not the
 * pointer -- so they landed at the absolute address cnt + offset rather than
 * inside the array. The plain `*(rr = ...) = ...` store used the right base;
 * only the uses of `rr` AFTER it were rewritten. -O0 and -O1 were correct.
 *
 * The casualty was toybox's shell: save_redirect() records its redirect undo
 * list exactly this way, so every `echo x > file` left the list zeroed and the
 * matching unredirect() then ran dup2(0, 0) + close(0) -- closing the shell's
 * own stdin, and scribbling two words over low memory on the way.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int *grow(int *old, int bytes)
{
  int *p = realloc(old, bytes);
  /* Zeroed so a miscompiled run reports a stable (0,0) instead of whatever the
   * allocator happened to leave behind. */
  memset(p, 0, bytes);
  return p;
}

static void record(int **rd, int hfd, int to)
{
  int cnt, *rr;

  if (!((cnt = *rd ? **rd : 0) & 31))
    *rd = grow(*rd, (cnt + 33) * 2 * sizeof(int));
  *(rr = *rd) = ++cnt;
  rr[2 * cnt - 1] = hfd;
  rr[2 * cnt] = to;
}

int main(void)
{
  int *urd = 0;

  record(&urd, 10, 1);
  printf("count=%d pair=(%d,%d)\n", urd[0], urd[1], urd[2]);
  /* A second record() exercises the non-realloc path, where `rr` comes from
   * the already-allocated list rather than straight out of grow(). */
  record(&urd, 12, 2);
  printf("count=%d pair=(%d,%d)\n", urd[0], urd[3], urd[4]);
  if (urd[0] == 2 && urd[1] == 10 && urd[2] == 1 && urd[3] == 12 && urd[4] == 2)
    printf("OK\n");
  else
    printf("FAIL\n");
  free(urd);
  return 0;
}
