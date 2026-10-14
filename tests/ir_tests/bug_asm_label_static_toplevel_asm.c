/*
 *  TCC IR - top-level asm naming a static symbol by its asm label
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* An asm reference to `real_target`, the asm label of static `target`, made
 * a second, undefined global `real_target` (link error).  asm_label_find now
 * falls back to the file-scope C symbol carrying that label, and a C symbol
 * labelled after the asm named it takes over the asm symbol's ELF entry. */

#include <stdio.h>

/* Named by asm before any C declaration. */
int viafirst(int x);
__asm__(".text\n.thumb\n.syntax unified\n.global viafirst\n.type viafirst,%function\n.thumb_func\n"
        "viafirst:\n b real_first\n");
static int __attribute__((noinline, used)) first(int x) __asm__("real_first");
static int __attribute__((noinline, used)) first(int x) { return x + 7; }

/* An 8-byte aggregate return reached from asm keeps the AAPCS convention. */
struct s { int a, b; };
static struct s __attribute__((noinline, used)) mk(int x) __asm__("real_mk");
static struct s __attribute__((noinline, used)) mk(int x) { struct s r = {x, x * 2}; return r; }
int use_mk(int x) { return mk(x).b; }
struct s callmk(int x);
__asm__(".text\n.thumb\n.syntax unified\n.global callmk\n.type callmk,%function\n.thumb_func\n"
        "callmk:\n b real_mk\n");

/* Named from a function's inline asm. */
static int inl(int x) __asm__("real_inl");
static int __attribute__((noinline, used)) inl(int x) { return x - 1; }
__attribute__((noinline)) int viainl(int x)
{
  int r;
  __asm__ volatile("mov r0, %1\n bl real_inl\n mov %0, r0" : "=r"(r) : "r"(x) : "r0", "r1", "r2", "r3", "lr", "cc",
                   "memory");
  return r;
}

static int __attribute__((noinline, used)) target(int x) __asm__("real_target");
static int __attribute__((noinline, used)) target(int x) { return x + 1; }

/* Declared with a label, referenced from asm before its definition. */
static int __attribute__((noinline, used)) later(int x) __asm__("real_later");

/* A static object reached through its label. */
static int counter __asm__("real_counter") = 40;

/* Neighbours: a global labelled function, an unlabelled static by C name. */
int glob(int x) __asm__("real_glob");
int glob(int x) { return x * 2; }
static int __attribute__((noinline, used)) plain(int x) { return x - 3; }

int viaasm(int x);
int vialater(int x);
int viaglob(int x);
int viaplain(int x);
int *counter_addr(void);
__asm__(".text\n.thumb\n.syntax unified\n"
        ".global viaasm\n.type viaasm,%function\n.thumb_func\nviaasm:\n b real_target\n"
        ".global vialater\n.type vialater,%function\n.thumb_func\nvialater:\n b real_later\n"
        ".global viaglob\n.type viaglob,%function\n.thumb_func\nviaglob:\n b real_glob\n"
        ".global viaplain\n.type viaplain,%function\n.thumb_func\nviaplain:\n b plain\n"
        ".global counter_addr\n.type counter_addr,%function\n.thumb_func\ncounter_addr:\n"
        " ldr r0, 1f\n bx lr\n .balign 4\n1: .word real_counter\n");

static int __attribute__((noinline, used)) later(int x) { return x * 10; }

int main(void)
{
  printf("%d\n", viaasm(10));
  printf("%d %d %d %d\n", viafirst(1), callmk(5).b, use_mk(3), viainl(10));
  printf("%d\n", vialater(4));
  printf("%d %d\n", viaglob(21), glob(1));
  printf("%d\n", viaplain(8));
  *counter_addr() += 2;
  printf("%d %d\n", counter, counter_addr() == &counter ? 1 : 0);
  return 0;
}
