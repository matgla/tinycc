/* Regression test: for-loop increment lost with nested ternary function arg.
 *
 * Exact pattern from tccpp_new():
 *   for (i = CH_EOF; i < 128; i++)
 *       set_idnum(i, is_space(i) ? IS_SPC : isid(i) ? IS_ID : isnum(i) ? IS_NUM : 0);
 *
 * At -O1, the loop increment (i++) was dropped from codegen, causing an
 * infinite loop.  The nested ternary ? : ? : ? : chain as a function/store
 * argument is the trigger.
 */
#include <stdio.h>

#define CH_EOF (-1)
#define IS_SPC 1
#define IS_ID 2
#define IS_NUM 4

static inline int is_space(int ch)
{
  return ch == ' ' || ch == '\t' || ch == '\v' || ch == '\f' || ch == '\r';
}
static inline int isid(int c)
{
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
static inline int isnum(int c)
{
  return c >= '0' && c <= '9';
}

static unsigned char isidnum_table[256 - CH_EOF];

int set_idnum(int c, int val)
{
  int prev = isidnum_table[c - CH_EOF];
  isidnum_table[c - CH_EOF] = val;
  return prev;
}

int main(void)
{
  int i;

  /* This is the exact pattern that triggered the bug in tccpp_new(). */
  for (i = CH_EOF; i < 128; i++)
    set_idnum(i, is_space(i) ? IS_SPC : isid(i) ? IS_ID : isnum(i) ? IS_NUM : 0);

  for (i = 128; i < 256; i++)
    set_idnum(i, IS_ID);

  /* Verify some representative entries */
  printf("space=%d id=%d num=%d other=%d\n", isidnum_table[' ' - CH_EOF], isidnum_table['A' - CH_EOF],
         isidnum_table['0' - CH_EOF], isidnum_table['@' - CH_EOF]);

  /* Verify the loop actually ran to completion */
  printf("last=%d\n", isidnum_table[127 - CH_EOF]);

  return 0;
}
