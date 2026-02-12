/*
 * Bug: A ternary operator inside a while loop preceding a sparse switch
 * causes TCC's IR backend to skip emitting the switch dispatch (case
 * comparison chain).  The switch case bodies are emitted but become dead
 * code because no dispatch reaches them.
 *
 * Root cause: gjmp() inside expr_cond() sets CODE_OFF_BIT in nocode_wanted.
 * tcc_ir_backpatch_to_here() did not clear it (unlike gsym() which calls
 * CODE_ON()), so the bit remained set through the while-loop exit and into
 * the switch entry.  The switch handler captures nocode_wanted at entry and
 * skips dispatch generation when it is non-zero.
 *
 * Pattern that triggers the bug:
 *   while (...) { x = x < 4 ? 1 : 2; }
 *   switch (c) { case 'd': ... case 'i': ... }
 *
 * Expected: switch dispatches correctly, prints "PASS"
 * Without fix: switch dispatch is missing, prints "FAIL"
 */
#include <stdio.h>

int test(const char *s)
{
  int n = 0;
  while (*s)
  {
    int c = (unsigned char)*s++;
    int bytes = 4;

    if (c != '%')
    {
      ++n;
      continue;
    }

    /* This while-loop with ternary triggers the bug */
    while (*s == 'h')
    {
      bytes = bytes < 4 ? 1 : 2;
      s++;
    }

    switch ((c = *s++))
    {
    case 'd':
    case 'i':
      n += bytes + 1;
      break;
    case 'u':
      n += bytes + 2;
      break;
    case 'o':
      n += bytes + 3;
      break;
    case 'p':
      n += 10;
      break;
    case 'x':
    case 'X':
      n += bytes + 4;
      break;
    case 'c':
      n += 1;
      break;
    case 's':
      n += 5;
      break;
    case 'n':
      n += 0;
      break;
    case '\0':
      return n;
    default:
      break;
    }
  }
  return n;
}

int main(void)
{
  int r;

  /* "A%d" -> 'A' adds 1, '%' enters format, no 'h' loop, 'd' adds 4+1=5 => 6 */
  r = test("A%d");
  printf("test1: %d %s\n", r, r == 6 ? "PASS" : "FAIL");

  /* "%s" -> '%' enters format, no 'h' loop, 's' adds 5 => 5 */
  r = test("%s");
  printf("test2: %d %s\n", r, r == 5 ? "PASS" : "FAIL");

  /* "%hhd" -> '%', two 'h' iterations:
     bytes starts at 4, first 'h': 4<4 false -> bytes=2, second 'h': 2<4 true -> bytes=1
     then 'd' adds 1+1=2 => 2 */
  r = test("%hhd");
  printf("test3: %d %s\n", r, r == 2 ? "PASS" : "FAIL");

  /* "AB%xCD" -> 'A' +1, 'B' +1, '%' format, 'x' adds 4+4=8, 'C' +1, 'D' +1 => 12 */
  r = test("AB%xCD");
  printf("test4: %d %s\n", r, r == 12 ? "PASS" : "FAIL");

  return 0;
}
