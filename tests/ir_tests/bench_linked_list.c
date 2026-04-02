#include <stdio.h>
#include <stddef.h>

struct node {
    int value;
    struct node *next;
};

static struct node nodes[100];

int bench_linked_list(void)
{
  int sum = 0;

  for (int i = 0; i < 100; i++) {
      nodes[i].value = i * 3 + 7;
      nodes[i].next = (i < 99) ? &nodes[i + 1] : NULL;
  }

  sum = 0;
  struct node *p = &nodes[0];
  while (p) {
      sum += p->value;
      p = p->next;
  }

  return sum;
}

int main(void)
{
    printf("linked_list: %d\n", bench_linked_list());
    return 0;
}
