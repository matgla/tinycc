/**
 * Regression test for static function pointer relocation under PIC with
 * text/data separation and -ffunction-sections.
 *
 * Bug: When compiled with -ffunction-sections, each function gets its own
 * .text.funcname section.  The PIC code generator incorrectly used
 * R_ARM_GOTOFF for static functions in other text sections.  GOTOFF
 * computes (symbol - GOT_addr) which only works when both are in the
 * same segment; with text/data separation, .text and .got are in
 * different segments so the offset is wrong at runtime, causing the
 * call to jump to a garbage address (e.g. _start instead of the
 * intended callback).
 *
 * This test must be compiled with: -ffunction-sections
 * Mimics the toybox llist_traverse() pattern that triggered the crash.
 */
#include <stdio.h>

/* A simple linked list node */
struct node
{
  struct node *next;
  int value;
};

/* Static callback function — will be in .text.accumulate with -ffunction-sections */
static int total = 0;
static void accumulate(void *data)
{
  struct node *n = (struct node *)data;
  total += n->value;
}

/* Traverse function in a different section — calls callback via pointer */
static void traverse(struct node *list, void (*func)(void *))
{
  while (list)
  {
    struct node *next = list->next;
    func(list);
    list = next;
  }
}

/* Another static function pointer test — store in variable first */
static int doubled_total = 0;
static void double_accumulate(void *data)
{
  struct node *n = (struct node *)data;
  doubled_total += n->value * 2;
}

int main(void)
{
  struct node c = {0, 30};
  struct node b = {&c, 20};
  struct node a = {&b, 10};

  /* Direct function pointer call through traverse */
  traverse(&a, accumulate);
  printf("total=%d\n", total);

  /* Store callback in variable, then call */
  void (*cb)(void *) = double_accumulate;
  traverse(&a, cb);
  printf("doubled=%d\n", doubled_total);

  /* Verify values */
  if (total != 60)
    return 1;
  if (doubled_total != 120)
    return 1;

  printf("ok\n");
  return 0;
}
