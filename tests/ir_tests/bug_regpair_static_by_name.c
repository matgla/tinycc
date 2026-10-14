/*
 *  TCC IR - r0/r1 aggregate returns: every way to reach a function by name
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* A static function returning an 8-byte aggregate takes it in r0/r1 on its
 * direct calls (gfunc_regpair_ret); everything else must reach an AAPCS
 * entry.  The shapes here once got the r0/r1 body, or none:
 *  - a static inline function inlined at its call and then address-taken:
 *    the adapter called a body that was never emitted (an undefined `f`);
 *  - asm naming the function directly, with or without `used`, before or
 *    after its definition and its first direct call;
 *  - a direct call compiled before the definition, which then adds `used`
 *    (at -O0 the caller is emitted before the attribute is seen). */

#include <stdio.h>

typedef struct { int a, b; } P;
typedef P (*FP)(int);

/* 1. static inline: called (and inlined), then its address taken. */
static inline P si(int x) { P p = {x + 1, x * 2}; return p; }
int __attribute__((noinline)) use_si(int x) { P r = si(x); return r.a + r.b + x * 4; }
FP __attribute__((noinline)) get_si(void) { return si; }

/* 2. asm branches to a static function by name; C calls it directly. */
static P __attribute__((noinline)) target(int x) { P p = {x + 1, x + 2}; return p; }
P via_asm(int x);
__asm__(".text\n.thumb\n.global via_asm\n.type via_asm,%function\n.thumb_func\nvia_asm:\n b target\n");
int __attribute__((noinline)) use_target(int x) { P r = target(x); return r.a * 10 + r.b; }

/* 3. A direct call before the definition, which adds `used`; asm calls it. */
static P late(int x);
int __attribute__((noinline)) use_late(int x) { P r = late(x); return r.a * 10 + r.b; }
static P __attribute__((used, noinline)) late(int x) { P p = {x + 3, x + 4}; return p; }
P via_asm_late(int x);
__asm__(".text\n.thumb\n.global via_asm_late\n.type via_asm_late,%function\n.thumb_func\nvia_asm_late:\n b late\n");

/* 4. asm names the function before C declares it at all. */
P via_asm_first(int x);
__asm__(".text\n.thumb\n.global via_asm_first\n.type via_asm_first,%function\n.thumb_func\nvia_asm_first:\n b early\n");
static P __attribute__((noinline)) early(int x) { P p = {x * 3, x - 1}; return p; }
int __attribute__((noinline)) use_early(int x) { P r = early(x); return r.a * 10 + r.b; }

/* 5. An address in a data word from asm, after the direct call. */
static P __attribute__((noinline)) dat(int x) { P p = {x - 5, x + 5}; return p; }
int __attribute__((noinline)) use_dat(int x) { P r = dat(x); return r.a * 10 + r.b; }
extern FP const dat_ptr;
__asm__(".section .rodata\n.balign 4\n.global dat_ptr\ndat_ptr:\n .word dat\n.text\n");

int main(void)
{
  int u = use_si(3);
  P a = get_si()(5);
  printf("si %d %d %d\n", u, a.a, a.b);

  P b = via_asm(10);
  printf("target %d %d %d\n", use_target(1), b.a, b.b);

  P c = via_asm_late(20);
  printf("late %d %d %d\n", use_late(1), c.a, c.b);

  P d = via_asm_first(7);
  printf("early %d %d %d\n", use_early(2), d.a, d.b);

  P e = dat_ptr(30);
  printf("dat %d %d %d\n", use_dat(6), e.a, e.b);
  return 0;
}
