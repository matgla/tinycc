"""The allocator behind the compiler's hot IR arrays, under a random workload.

It only ever runs on the device, where a bad block shows up as a miscompile much
later, so it is exercised here against a shadow copy of every live block.
"""

import subprocess
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]

DRIVER = r"""
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "memory/hot_arena.h"

#define ARENA (64 * 1024)
#define SLOTS 64
static _Alignas(8) char memory[ARENA];
static struct { unsigned char *p; size_t size; unsigned char fill; } slot[SLOTS];
static HotArena a;

static void check(int i)
{
  for (size_t k = 0; k < slot[i].size; k++)
    assert(slot[i].p[k] == slot[i].fill);
}

int main(int argc, char **argv)
{
  srand((unsigned)atoi(argv[1]));
  hot_arena_init(&a, memory, ARENA);
  unsigned long allocs = 0, grown = 0, full = 0;
  for (int op = 0; op < 200000; op++)
  {
    int i = rand() % SLOTS;
    /* Mostly small, as IR arrays of small functions are, and now and then huge. */
    size_t size = rand() % 16 == 0 ? (size_t)(rand() % (ARENA / 2)) : (size_t)(rand() % 2048);
    if (!slot[i].p)
    {
      unsigned char *p = hot_arena_alloc(&a, size);
      if (!p) { full++; continue; }
      assert(((unsigned long)p & 7) == 0);
      assert(hot_arena_contains(&a, p));
      assert(hot_arena_contains(&a, p + size - (size != 0)));
      assert(hot_arena_block_size(p) >= size);
      slot[i].p = p;
      slot[i].size = size;
      slot[i].fill = (unsigned char)rand();
      memset(p, slot[i].fill, size);
      allocs++;
    }
    else if (rand() % 3 == 0 && hot_arena_resize(&a, slot[i].p, size))
    {
      size_t keep = size < slot[i].size ? size : slot[i].size;
      for (size_t k = 0; k < keep; k++)
        assert(slot[i].p[k] == slot[i].fill);
      memset(slot[i].p, slot[i].fill, size);
      slot[i].size = size;
      grown++;
    }
    else
    {
      check(i);
      hot_arena_free(&a, slot[i].p);
      slot[i].p = NULL;
    }
    if (op % 1024 == 0)
      for (int j = 0; j < SLOTS; j++)
        if (slot[j].p)
          check(j);
  }
  for (int i = 0; i < SLOTS; i++)
    if (slot[i].p)
    {
      check(i);
      hot_arena_free(&a, slot[i].p);
    }
  /* Everything free again: it must have coalesced back into one block. */
  assert(a.used == 0);
  void *all = hot_arena_alloc(&a, ARENA - 8);
  assert(all);
  hot_arena_free(&a, all);
  assert(allocs > 1000 && grown > 100 && full > 0);
  puts("ok");
  return 0;
}
"""


@pytest.mark.parametrize("seed", [1, 2, 3, 4])
def test_random_workload_keeps_every_block_intact(tmp_path, seed):
    src, exe = tmp_path / "driver.c", tmp_path / "driver"
    src.write_text(DRIVER)
    subprocess.run(
        ["gcc", "-std=c11", "-O1", "-g", "-Wall", "-Werror", f"-I{ROOT}/source/memory/include",
         str(src), str(ROOT / "source/memory/hot_arena.c"), "-o", str(exe)],
        check=True, capture_output=True, text=True,
    )
    r = subprocess.run([str(exe), str(seed)], capture_output=True, text=True, timeout=120)
    assert r.returncode == 0 and r.stdout.strip() == "ok", r.stdout + r.stderr
