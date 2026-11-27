/* Chapter 2 -- ownership: owned, view, maybe, out, drop (README.md).
 *
 * A list of names.  The list owns its nodes and each node owns its text;
 * functions that only look at the list borrow it (view); a function that
 * frees it says so (drop).  Build: tcc 02_ownership.c mem.o -o names */
#pragma yasc
#include <stdio.h>
#include <string.h>
#include "mem.h"

struct name
{
  struct name *?owned next; /* the rest of the list, or null */
  char *owned text;              /* never null once the node exists */
};

/* Takes ownership of `head` and `text` (owned parameters) and returns the new
   head: both end up in the new node -- or, out of memory, text is freed and
   the old head comes back. */
static struct name *?owned push(struct name *?owned head, char *owned text)
{
  struct name *?owned n = mem_new(sizeof *n);
  if (!n)
  {
    mem_free(text);
    return head;
  }
  n->text = move(text);
  n->next = move(head);
  return n;
}

/* Only looks: the list is borrowed (view) and may be empty (maybe).  `out`
   says the function writes *chars before it returns. */
static int count(struct name *?list, out int *chars)
{
  int n = 0;
  *chars = 0;
  for (; list; list = list->next)
  {
    n++;
    *chars += cast<int(int)>(strlen(list->text));
  }
  return n;
}

/* drop: the list is consumed -- every node and every text is freed here. */
static void free_all(drop struct name *?owned list)
{
  while (list)
  {
    struct name *?owned next = move(list->next);
    mem_free(list->text);
    mem_free(list);
    list = move(next);
  }
}

int main(void)
{
  const char *words[] = {"ada", "grace", "barbara"};
  struct name *?owned list = nullptr;
  int chars;

  for (int i = 0; i < 3; i++)
  {
    char *?owned text = mem_strdup(words[i]);
    if (!text)
      break;
    list = push(move(list), move(text));
  }
  printf("%d names, %d chars\n", count(list, &chars), chars);
  for (const struct name *p = list; p; p = p->next)
    printf("  %s\n", p->text);
  free_all(list);
  printf("blocks left: %d\n", mem_live());
  return 0;
}
