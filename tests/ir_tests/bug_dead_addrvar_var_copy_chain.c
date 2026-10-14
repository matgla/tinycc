/*
 *  TCC IR - A dead local's address copied VAR to VAR before the store through it
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* After inlining, &pp reaches `*pp = p` through V4 <-- T(&pp); V6 <-- V4;
 * T' <-- V6.  dead_addrvar did not follow the VAR-to-VAR copy, deleted the
 * LEA and kept the store, which then wrote through whatever register held
 * the deleted TEMP (gcc 990128-1: head->n here, argc on the device). */

#include <stdio.h>

struct s
{
  struct s *n;
};
static struct s nodes[4];
static int count;

static void look(struct s *p, struct s **pp)
{
  for (; p; p = p->n)
    ;
  *pp = p;
  count++;
}

static void sub(struct s *p, struct s **pp)
{
  for (;;)
  {
    look(p, pp);
    if (p)
      p = p->n;
    else
      break;
  }
}

/* pp is never read: its stores may go, but not through a stray register
 * (the unused first argument puts head where the stray store looked). */
__attribute__((noinline)) int walk_dead(int *unused, struct s *head)
{
  struct s *pp;
  sub(head, &pp);
  return count;
}

/* pp is read afterwards: the stores through the copies must stay. */
__attribute__((noinline)) int walk_live(struct s *head)
{
  struct s *pp = head;
  sub(head, &pp);
  return pp == 0;
}

static void link_nodes(void)
{
  for (int i = 0; i < 3; i++)
    nodes[i].n = &nodes[i + 1];
  nodes[3].n = 0;
}

static int chain_len(void)
{
  int k = 0;
  for (struct s *p = &nodes[0]; p; p = p->n)
    k++;
  return k;
}

int main(void)
{
  link_nodes();
  count = 0;
  int c = walk_dead(0, &nodes[0]);
  printf("dead: count=%d chain=%d\n", c, chain_len());

  link_nodes();
  count = 0;
  int z = walk_live(&nodes[0]);
  printf("live: count=%d null=%d chain=%d\n", count, z, chain_len());
  return 0;
}
