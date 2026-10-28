/*
 *  TCC Tests - the loop counter of tcc_parse_args, a parameter bumped in the body
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */
/* tcc_parse_args' shape: `optind` is spilled, bumped on most paths, and the
 * `@file` path continues without touching it.  ra:loop_split built a loop
 * from one back edge's blocks, so for that latch optind had no def in the
 * loop: the header read an entry copy no other latch refreshed, and the loop
 * reparsed argv[1] forever.  A loop is now its header's whole natural loop. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct opt { const char *name; unsigned short index; unsigned short flags; };
struct state { unsigned char verbose; unsigned char a, b, c; int out; int sum; int nfiles; const char *files[8]; };

enum { O_HELP = 1, O_c, O_o, O_v, O_a, O_b, O_cc, O_I, O_D };
#define HAS_ARG 1
#define NOSEP 2

static const struct opt options[] = {
  {"h", O_HELP, 0}, {"c", O_c, 0}, {"o", O_o, HAS_ARG}, {"v", O_v, HAS_ARG | NOSEP},
  {"a", O_a, 0}, {"b", O_b, 0}, {"C", O_cc, 0}, {"I", O_I, HAS_ARG}, {"D", O_D, HAS_ARG},
  {NULL, 0, 0},
};

static int steps;

/* Stops a miscompiled loop that never advances its counter. */
static void step(void)
{
  if (++steps > 64)
  {
    printf("runaway loop\n");
    exit(1);
  }
}

static int strstart(const char *val, const char **str)
{
  const char *p = *str, *q = val;
  while (*q)
  {
    if (*p != *q)
      return 0;
    p++;
    q++;
  }
  *str = p;
  return 1;
}

static __attribute__((noinline)) void add_file(struct state *s, const char *f)
{
  step();
  if (s->nfiles < 8)
    s->files[s->nfiles++] = f;
}

static __attribute__((noinline)) int listfile(struct state *s, const char *f, int optind, int *pargc)
{
  step();
  (void)s;
  (void)optind;
  if (!strcmp(f, "bad"))
    return 1;
  --*pargc;
  return 0;
}

__attribute__((noinline)) int parse_args(struct state *s, int *pargc, char ***pargv, int optind)
{
  const struct opt *popt;
  const char *optarg, *r;
  int tool = 0, noaction = optind;
  int n_c = 0, n_o = 0, n_v = 0, n_a = 0, n_b = 0, n_i = 0, n_d = 0, n_f = 0;
  char **argv = *pargv;
  int argc = *pargc;

  while (optind < argc)
  {
    r = argv[optind];
    step();
    if (r[0] == '@' && r[1] != '\0')
    {
      if (listfile(s, r + 1, optind, &argc))
        return -1;
      continue;
    }
    optind++;
    if (tool)
    {
      if (r[0] == '-' && r[1] == 'v' && r[2] == 0)
        ++s->verbose;
      continue;
    }
    if (r[0] != '-' || r[1] == '\0')
    {
      add_file(s, r);
      n_f += optind;
      continue;
    }
    for (popt = options;; ++popt)
    {
      const char *p1 = popt->name;
      const char *r1 = r + 1;
      if (p1 == NULL)
      {
        printf("invalid option -- '%s'\n", r);
        return -1;
      }
      if (!strstart(p1, &r1))
        continue;
      optarg = r1;
      if (popt->flags & HAS_ARG)
      {
        if (*r1 == '\0' && !(popt->flags & NOSEP))
        {
          if (optind >= argc)
          {
            printf("argument to '%s' is missing\n", r);
            return -1;
          }
          optarg = argv[optind++];
        }
      }
      else if (*r1 != '\0')
        continue;
      break;
    }
    switch (popt->index)
    {
    case O_HELP:
      return 2;
    case O_c:
      s->out = 1;
      n_c += optind;
      break;
    case O_o:
      add_file(s, optarg);
      ++noaction;
      n_o += argc;
      break;
    case O_v:
      do
        ++s->verbose;
      while (*optarg++ == 'v');
      ++noaction;
      n_v += 3;
      break;
    case O_a:
      s->a = 1;
      n_a += n_v + 1;
      break;
    case O_b:
      s->b = 1;
      n_b += n_a + 2;
      break;
    case O_cc:
      tool = 1;
      break;
    case O_I:
      n_i += optind * 5;
      add_file(s, optarg);
      break;
    case O_D:
      n_d += n_i + 7;
      add_file(s, optarg);
      break;
    default:
      break;
    }
  }
  *pargc = argc;
  s->sum = n_c * 3 + n_o * 5 + n_v * 7 + n_a * 11 + n_b * 13 + n_i * 17 + n_d * 19 + n_f * 23;
  return optind - noaction;
}

static void run(const char *name, int argc, char **argv)
{
  struct state s;
  int r;
  memset(&s, 0, sizeof s);
  steps = 0;
  r = parse_args(&s, &argc, &argv, 1);
  printf("%s: r=%d verbose=%d out=%d a=%d b=%d sum=%d files=%d", name, r, s.verbose, s.out, s.a, s.b, s.sum, s.nfiles);
  for (int i = 0; i < s.nfiles; i++)
    printf(" %s", s.files[i]);
  printf("\n");
}

int main(void)
{
  char *a1[] = {"tcc", "-v", NULL};
  char *a2[] = {"tcc", "-c", "x.c", "-o", "x.o", NULL};
  char *a3[] = {"tcc", "-vvv", "-a", "-b", "-Iinc", "-D", "N", NULL};
  char *a4[] = {"tcc", "-C", "-v", "-v", "y", NULL};
  char *a5[] = {"tcc", "@list", "z.c", NULL};
  run("v", 2, a1);
  run("c-o", 5, a2);
  run("vvv", 7, a3);
  run("tool", 5, a4);
  run("list", 3, a5);
  return 0;
}
