/*
 *  TCC IR - a static chain is built from a real frame and survives the call
 *
 *  Copyright (c) 2026 Mateusz Stadnik
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 */

/* SET_CHAIN passes R7 + frame size.  Base f6d53136 dropped the frame pointer
 * of a parent whose inline-marked nested function kept a recursive call
 * (-O1+), gave a capture-less nested function no frame and no saved chain, so
 * a child call's chain leaked into the next sibling call (-O0), and set R10
 * before the arguments were evaluated, so an argument reading a capture or
 * making a call saw the wrong chain; a trampoline made inside a nested
 * function always got that function's own frame.  Fix: every nested function
 * keeps its incoming chain, a function with a chain op keeps its frame
 * pointer, SET_CHAIN takes effect at the branch, and both chain ops pass the
 * callee's defining frame (nested_chain_level). */
#include <stdio.h>

__attribute__((noinline)) int ext(int x) { return x * 2; }

/* -O1+: the recursive call left by the inliner's cycle check */
__attribute__((noinline)) int self_recursive(int k)
{
  int g(int n) { return n > 100 ? n + k : g(n + 50) + 1; }
  return g(3) * 10 + g(1);
}

/* -O0: n captures nothing itself; w's chain must not reach the call of q */
__attribute__((noinline)) int f2(int k)
{
  int q(int x) { return x + 100 + k; }
  int n(int y)
  {
    int r = q(y);
    {
      int w(int x) { return x + 7; }
      r += w(y) * 1000;
    }
    return r + q(0);
  }
  return n(1);
}

/* arguments read a capture and make calls after the chain is chosen */
__attribute__((noinline)) int f3(int k)
{
  int n(int y)
  {
    int loc = y + 1;
    __attribute__((noinline)) int w(int x) { return x + loc * 10; }
    int r = w(k);
    r += w(ext(k)) * 100;
    r += w(w(k) + loc) * 10000;
    return r;
  }
  return n(5);
}

/* the argument calls a sibling through a function pointer (a trampoline) */
int (*volatile sp)(int);
__attribute__((noinline)) int f4(int k)
{
  int sib(int x) { return x + k; }
  sp = sib;
  int n(int y)
  {
    __attribute__((noinline)) int w(int x) { return x * 3 + y; }
    return w(sp(y)) + sp(1);
  }
  return n(4);
}

/* a call in return position of a parent */
__attribute__((noinline)) int f5(int k)
{
  __attribute__((noinline)) int t(int x) { return x - k; }
  return t(k * 7);
}

/* a trampoline made inside a nested function for a sibling or an ancestor
 * carries that function's chain, not this frame */
__attribute__((noinline)) int call(int (*fn)(int), int x) { return fn(x); }
__attribute__((noinline)) int f6(int k)
{
  int sib(int x) { return x + k; }
  int n(int y)
  {
    int z = y * 2;
    int w(int x) { return x + z; }
    return call(sib, y) * 100 + call(w, 1);
  }
  return n(4);
}
__attribute__((noinline)) int f7(int k)
{
  int outer(int a)
  {
    int inner(int m) { return call(outer, m - 1) + k; }
    return a <= 0 ? 1000 : call(inner, a);
  }
  return outer(2);
}

int main(void)
{
  printf("rec %d %d\n", self_recursive(0), self_recursive(5));
  printf("f2 %d\n", f2(0));
  printf("f3 %d\n", f3(3));
  printf("f4 %d\n", f4(2));
  printf("f5 %d\n", f5(5));
  printf("f6 %d f7 %d\n", f6(3), f7(5));
  return 0;
}
